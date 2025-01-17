/***************************************************************************
                         qgsvectorlayerprofilegenerator.cpp
                         ---------------
    begin                : March 2022
    copyright            : (C) 2022 by Nyall Dawson
    email                : nyall dot dawson at gmail dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgsvectorlayerprofilegenerator.h"
#include "qgsprofilerequest.h"
#include "qgscurve.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerelevationproperties.h"
#include "qgscoordinatetransform.h"
#include "qgsgeos.h"
#include "qgsvectorlayerfeatureiterator.h"
#include "qgsterrainprovider.h"
#include "qgspolygon.h"
#include "qgstessellator.h"
#include "qgsmultipolygon.h"
#include "qgsmeshlayerutils.h"
#include "qgsmultipoint.h"
#include "qgsmultilinestring.h"
#include "qgslinesymbol.h"
#include "qgsfillsymbol.h"
#include "qgsmarkersymbol.h"
#include <QPolygonF>

//
// QgsVectorLayerProfileResults
//

QString QgsVectorLayerProfileResults::type() const
{
  return QStringLiteral( "vector" );
}

QMap<double, double> QgsVectorLayerProfileResults::distanceToHeightMap() const
{
  return mDistanceToHeightMap;
}

QgsDoubleRange QgsVectorLayerProfileResults::zRange() const
{
  return QgsDoubleRange( minZ, maxZ );
}

QgsPointSequence QgsVectorLayerProfileResults::sampledPoints() const
{
  return rawPoints;
}

QVector<QgsGeometry> QgsVectorLayerProfileResults::asGeometries() const
{
  return geometries;
}

void QgsVectorLayerProfileResults::renderResults( QgsProfileRenderContext &context )
{
  QPainter *painter = context.renderContext().painter();
  if ( !painter )
    return;

  const QgsScopedQPainterState painterState( painter );

  painter->setBrush( Qt::NoBrush );
  painter->setPen( Qt::NoPen );

  const double minDistance = context.distanceRange().lower();
  const double maxDistance = context.distanceRange().upper();
  const double minZ = context.elevationRange().lower();
  const double maxZ = context.elevationRange().upper();

  const QRectF visibleRegion( minDistance, minZ, maxDistance - minDistance, maxZ - minZ );
  QPainterPath clipPath;
  clipPath.addPolygon( context.worldTransform().map( visibleRegion ) );
  painter->setClipPath( clipPath, Qt::ClipOperation::IntersectClip );

  const QgsRectangle clipPathRect( clipPath.boundingRect() );

  profileMarkerSymbol->startRender( context.renderContext() );
  profileFillSymbol->startRender( context.renderContext() );
  profileLineSymbol->startRender( context.renderContext() );

  for ( const QgsGeometry &geometry : std::as_const( distanceVHeightGeometries ) )
  {
    if ( geometry.isEmpty() )
      continue;

    QgsGeometry transformed = geometry;
    transformed.transform( context.worldTransform() );

    if ( !transformed.boundingBoxIntersects( clipPathRect ) )
      continue;

    // we can take some shortcuts here, because we know that the geometry will already be segmentized and can't be a curved type
    switch ( transformed.type() )
    {
      case QgsWkbTypes::PointGeometry:
      {
        if ( const QgsPoint *point = qgsgeometry_cast< const QgsPoint * >( transformed.constGet() ) )
        {
          profileMarkerSymbol->renderPoint( QPointF( point->x(), point->y() ), nullptr, context.renderContext() );
        }
        else if ( const QgsMultiPoint *multipoint = qgsgeometry_cast< const QgsMultiPoint * >( transformed.constGet() ) )
        {
          const int numGeometries = multipoint->numGeometries();
          for ( int i = 0; i < numGeometries; ++i )
          {
            profileMarkerSymbol->renderPoint( QPointF( multipoint->pointN( i )->x(), multipoint->pointN( i )->y() ), nullptr, context.renderContext() );
          }
        }
        break;
      }

      case QgsWkbTypes::LineGeometry:
      {
        if ( const QgsLineString *line = qgsgeometry_cast< const QgsLineString * >( transformed.constGet() ) )
        {
          profileLineSymbol->renderPolyline( line->asQPolygonF(), nullptr, context.renderContext() );
        }
        else if ( const QgsMultiLineString *multiLinestring = qgsgeometry_cast< const QgsMultiLineString * >( transformed.constGet() ) )
        {
          const int numGeometries = multiLinestring->numGeometries();
          for ( int i = 0; i < numGeometries; ++i )
          {
            profileLineSymbol->renderPolyline( multiLinestring->lineStringN( i )->asQPolygonF(), nullptr, context.renderContext() );
          }
        }
        break;
      }

      case QgsWkbTypes::PolygonGeometry:
      {
        if ( const QgsPolygon *polygon = qgsgeometry_cast< const QgsPolygon * >( transformed.constGet() ) )
        {
          if ( const QgsCurve *exterior = polygon->exteriorRing() )
            profileFillSymbol->renderPolygon( exterior->asQPolygonF(), nullptr, nullptr, context.renderContext() );
        }
        else if ( const QgsMultiPolygon *multiPolygon = qgsgeometry_cast< const QgsMultiPolygon * >( transformed.constGet() ) )
        {
          const int numGeometries = multiPolygon->numGeometries();
          for ( int i = 0; i < numGeometries; ++i )
          {
            profileFillSymbol->renderPolygon( multiPolygon->polygonN( i )->exteriorRing()->asQPolygonF(), nullptr, nullptr, context.renderContext() );
          }
        }
        break;
      }

      case QgsWkbTypes::UnknownGeometry:
      case QgsWkbTypes::NullGeometry:
        continue;
    }

    transformed.constGet()->draw( *painter );
  }

  profileMarkerSymbol->stopRender( context.renderContext() );
  profileFillSymbol->stopRender( context.renderContext() );
  profileLineSymbol->stopRender( context.renderContext() );
}

//
// QgsVectorLayerProfileGenerator
//

QgsVectorLayerProfileGenerator::QgsVectorLayerProfileGenerator( QgsVectorLayer *layer, const QgsProfileRequest &request )
  : mFeedback( std::make_unique< QgsFeedback >() )
  , mProfileCurve( request.profileCurve() ? request.profileCurve()->clone() : nullptr )
  , mTerrainProvider( request.terrainProvider() ? request.terrainProvider()->clone() : nullptr )
  , mTolerance( request.tolerance() )
  , mSourceCrs( layer->crs() )
  , mTargetCrs( request.crs() )
  , mTransformContext( request.transformContext() )
  , mExtent( layer->extent() )
  , mSource( std::make_unique< QgsVectorLayerFeatureSource >( layer ) )
  , mOffset( layer->elevationProperties()->zOffset() )
  , mScale( layer->elevationProperties()->zScale() )
  , mClamping( qgis::down_cast< QgsVectorLayerElevationProperties* >( layer->elevationProperties() )->clamping() )
  , mBinding( qgis::down_cast< QgsVectorLayerElevationProperties* >( layer->elevationProperties() )->binding() )
  , mExtrusionEnabled( qgis::down_cast< QgsVectorLayerElevationProperties* >( layer->elevationProperties() )->extrusionEnabled() )
  , mExtrusionHeight( qgis::down_cast< QgsVectorLayerElevationProperties* >( layer->elevationProperties() )->extrusionHeight() )
  , mWkbType( layer->wkbType() )
  , mProfileLineSymbol( qgis::down_cast< QgsVectorLayerElevationProperties* >( layer->elevationProperties() )->profileLineSymbol()->clone() )
  , mProfileFillSymbol( qgis::down_cast< QgsVectorLayerElevationProperties* >( layer->elevationProperties() )->profileFillSymbol()->clone() )
  , mProfileMarkerSymbol( qgis::down_cast< QgsVectorLayerElevationProperties* >( layer->elevationProperties() )->profileMarkerSymbol()->clone() )
{
  if ( mTerrainProvider )
    mTerrainProvider->prepare(); // must be done on main thread

}

QgsVectorLayerProfileGenerator::~QgsVectorLayerProfileGenerator() = default;

bool QgsVectorLayerProfileGenerator::generateProfile()
{
  if ( !mProfileCurve || mFeedback->isCanceled() )
    return false;

  // we need to transform the profile curve to the vector's CRS
  mTransformedCurve.reset( mProfileCurve->clone() );
  mLayerToTargetTransform = QgsCoordinateTransform( mSourceCrs, mTargetCrs, mTransformContext );
  if ( mTerrainProvider )
    mTargetToTerrainProviderTransform = QgsCoordinateTransform( mTargetCrs, mTerrainProvider->crs(), mTransformContext );

  try
  {
    mTransformedCurve->transform( mLayerToTargetTransform, Qgis::TransformDirection::Reverse );
  }
  catch ( QgsCsException & )
  {
    QgsDebugMsg( QStringLiteral( "Error transforming profile line to vector CRS" ) );
    return false;
  }

  const QgsRectangle profileCurveBoundingBox = mTransformedCurve->boundingBox();
  if ( !profileCurveBoundingBox.intersects( mExtent ) )
    return false;

  if ( mFeedback->isCanceled() )
    return false;

  mResults = std::make_unique< QgsVectorLayerProfileResults >();

  mResults->profileLineSymbol.reset( mProfileLineSymbol->clone() );
  mResults->profileFillSymbol.reset( mProfileFillSymbol->clone() );
  mResults->profileMarkerSymbol.reset( mProfileMarkerSymbol->clone() );

  mProfileCurveEngine.reset( new QgsGeos( mProfileCurve.get() ) );
  mProfileCurveEngine->prepareGeometry();

  if ( mFeedback->isCanceled() )
    return false;

  switch ( QgsWkbTypes::geometryType( mWkbType ) )
  {
    case QgsWkbTypes::PointGeometry:
      if ( !generateProfileForPoints() )
        return false;
      break;

    case QgsWkbTypes::LineGeometry:
      if ( !generateProfileForLines() )
        return false;
      break;

    case QgsWkbTypes::PolygonGeometry:
      if ( !generateProfileForPolygons() )
        return false;
      break;

    case QgsWkbTypes::UnknownGeometry:
    case QgsWkbTypes::NullGeometry:
      return false;
  }

  return true;
}

QgsAbstractProfileResults *QgsVectorLayerProfileGenerator::takeResults()
{
  return mResults.release();
}

QgsFeedback *QgsVectorLayerProfileGenerator::feedback() const
{
  return mFeedback.get();
}

bool QgsVectorLayerProfileGenerator::generateProfileForPoints()
{
  // get features from layer
  QgsFeatureRequest request;
  request.setDestinationCrs( mTargetCrs, mTransformContext );
  request.setDistanceWithin( QgsGeometry( mProfileCurve->clone() ), mTolerance );
  request.setNoAttributes();
  request.setFeedback( mFeedback.get() );

  auto processPoint = [this]( const QgsPoint * point )
  {
    const double height = featureZToHeight( point->x(), point->y(), point->z() );
    mResults->rawPoints.append( QgsPoint( point->x(), point->y(), height ) );
    mResults->minZ = std::min( mResults->minZ, height );
    mResults->maxZ = std::max( mResults->maxZ, height );

    QString lastError;
    const double distance = mProfileCurveEngine->lineLocatePoint( *point, &lastError );
    mResults->mDistanceToHeightMap.insert( distance, height );

    if ( mExtrusionEnabled )
    {
      mResults->geometries.append( QgsGeometry( new QgsLineString( QgsPoint( point->x(), point->y(), height ),
                                   QgsPoint( point->x(), point->y(), height + mExtrusionHeight ) ) ) );
      mResults->distanceVHeightGeometries.append( QgsGeometry( new QgsLineString( QgsPoint( distance, height ),
          QgsPoint( distance, height + mExtrusionHeight ) ) ) );
      mResults->minZ = std::min( mResults->minZ, height + mExtrusionHeight );
      mResults->maxZ = std::max( mResults->maxZ, height + mExtrusionHeight );
    }
    else
    {
      mResults->geometries.append( QgsGeometry( new QgsPoint( point->x(), point->y(), height ) ) );
      mResults->distanceVHeightGeometries.append( QgsGeometry( new QgsPoint( distance, height ) ) );
    }
  };

  QgsFeature feature;
  QgsFeatureIterator it = mSource->getFeatures( request );
  while ( it.nextFeature( feature ) )
  {
    if ( mFeedback->isCanceled() )
      return false;

    const QgsGeometry g = feature.geometry();
    if ( g.isMultipart() )
    {
      for ( auto it = g.const_parts_begin(); it != g.const_parts_end(); ++it )
      {
        processPoint( qgsgeometry_cast< const QgsPoint * >( *it ) );
      }
    }
    else
    {
      processPoint( qgsgeometry_cast< const QgsPoint * >( g.constGet() ) );
    }
  }
  return true;
}

bool QgsVectorLayerProfileGenerator::generateProfileForLines()
{
  // get features from layer
  QgsFeatureRequest request;
  request.setDestinationCrs( mTargetCrs, mTransformContext );
  request.setFilterRect( mProfileCurve->boundingBox() );
  request.setNoAttributes();
  request.setFeedback( mFeedback.get() );

  auto processCurve = [this]( const QgsCurve * curve )
  {
    QString error;
    std::unique_ptr< QgsAbstractGeometry > intersection( mProfileCurveEngine->intersection( curve, &error ) );
    if ( !intersection )
      return;

    if ( mFeedback->isCanceled() )
      return;

    QgsGeos curveGeos( curve );
    curveGeos.prepareGeometry();

    if ( mFeedback->isCanceled() )
      return;

    for ( auto it = intersection->const_parts_begin(); it != intersection->const_parts_end(); ++it )
    {
      if ( mFeedback->isCanceled() )
        return;

      if ( const QgsPoint *intersectionPoint = qgsgeometry_cast< const QgsPoint * >( *it ) )
      {
        // unfortunately we need to do some work to interpolate the z value for the line -- GEOS doesn't give us this
        const double distance = curveGeos.lineLocatePoint( *intersectionPoint, &error );
        std::unique_ptr< QgsPoint > interpolatedPoint( curve->interpolatePoint( distance ) );

        const double height = featureZToHeight( interpolatedPoint->x(), interpolatedPoint->y(), interpolatedPoint->z() );
        mResults->rawPoints.append( QgsPoint( interpolatedPoint->x(), interpolatedPoint->y(), height ) );
        mResults->minZ = std::min( mResults->minZ, height );
        mResults->maxZ = std::max( mResults->maxZ, height );

        const double distanceAlongProfileCurve = mProfileCurveEngine->lineLocatePoint( *interpolatedPoint, &error );
        mResults->mDistanceToHeightMap.insert( distanceAlongProfileCurve, height );

        if ( mExtrusionEnabled )
        {
          mResults->geometries.append( QgsGeometry( new QgsLineString( QgsPoint( interpolatedPoint->x(), interpolatedPoint->y(), height ),
                                       QgsPoint( interpolatedPoint->x(), interpolatedPoint->y(), height + mExtrusionHeight ) ) ) );
          mResults->distanceVHeightGeometries.append( QgsGeometry( new QgsLineString( QgsPoint( distanceAlongProfileCurve, height ),
              QgsPoint( distanceAlongProfileCurve, height + mExtrusionHeight ) ) ) );
          mResults->minZ = std::min( mResults->minZ, height + mExtrusionHeight );
          mResults->maxZ = std::max( mResults->maxZ, height + mExtrusionHeight );
        }
        else
        {
          mResults->geometries.append( QgsGeometry( new QgsPoint( interpolatedPoint->x(), interpolatedPoint->y(), height ) ) );
          mResults->distanceVHeightGeometries.append( QgsGeometry( new QgsPoint( distanceAlongProfileCurve, height ) ) );
        }
      }
    }
  };

  QgsFeature feature;
  QgsFeatureIterator it = mSource->getFeatures( request );
  while ( it.nextFeature( feature ) )
  {
    if ( mFeedback->isCanceled() )
      return false;

    if ( !mProfileCurveEngine->intersects( feature.geometry().constGet() ) )
      continue;

    const QgsGeometry g = feature.geometry();
    if ( g.isMultipart() )
    {
      for ( auto it = g.const_parts_begin(); it != g.const_parts_end(); ++it )
      {
        if ( !mProfileCurveEngine->intersects( *it ) )
          continue;

        processCurve( qgsgeometry_cast< const QgsCurve * >( *it ) );
      }
    }
    else
    {
      processCurve( qgsgeometry_cast< const QgsCurve * >( g.constGet() ) );
    }
  }
  return true;
}

bool QgsVectorLayerProfileGenerator::generateProfileForPolygons()
{
  // get features from layer
  QgsFeatureRequest request;
  request.setDestinationCrs( mTargetCrs, mTransformContext );
  request.setFilterRect( mProfileCurve->boundingBox() );
  request.setNoAttributes();
  request.setFeedback( mFeedback.get() );

  auto interpolatePointOnTriangle = []( const QgsPolygon * triangle, double x, double y ) -> QgsPoint
  {
    QgsPoint p1, p2, p3;
    Qgis::VertexType vt;
    triangle->exteriorRing()->pointAt( 0, p1, vt );
    triangle->exteriorRing()->pointAt( 1, p2, vt );
    triangle->exteriorRing()->pointAt( 2, p3, vt );
    const double z = QgsMeshLayerUtils::interpolateFromVerticesData( p1, p2, p3, p1.z(), p2.z(), p3.z(), QgsPointXY( x, y ) );
    return QgsPoint( x, y, z );
  };

  std::function< void( const QgsPolygon *triangle, const QgsAbstractGeometry *intersect, QVector< QgsGeometry > & ) > processTriangleLineIntersect;
  processTriangleLineIntersect = [this, &interpolatePointOnTriangle, &processTriangleLineIntersect]( const QgsPolygon * triangle, const QgsAbstractGeometry * intersect, QVector< QgsGeometry > &transformedParts )
  {
    // intersect may be a (multi)point or (multi)linestring
    switch ( QgsWkbTypes::geometryType( intersect->wkbType() ) )
    {
      case QgsWkbTypes::PointGeometry:
        if ( const QgsMultiPoint *mp = qgsgeometry_cast< const QgsMultiPoint * >( intersect ) )
        {
          const int numPoint = mp->numGeometries();
          for ( int i = 0; i < numPoint; ++i )
          {
            processTriangleLineIntersect( triangle, mp->geometryN( i ), transformedParts );
          }
        }
        else if ( const QgsPoint *p = qgsgeometry_cast< const QgsPoint * >( intersect ) )
        {
          const QgsPoint interpolatedPoint = interpolatePointOnTriangle( triangle, p->x(), p->y() );
          mResults->rawPoints.append( interpolatedPoint );
          mResults->minZ = std::min( mResults->minZ, interpolatedPoint.z() );
          mResults->maxZ = std::max( mResults->maxZ, interpolatedPoint.z() );

          QString lastError;
          const double distance = mProfileCurveEngine->lineLocatePoint( *p, &lastError );
          mResults->mDistanceToHeightMap.insert( distance, interpolatedPoint.z() );

          if ( mExtrusionEnabled )
          {
            mResults->geometries.append( QgsGeometry( new QgsLineString( interpolatedPoint,
                                         QgsPoint( interpolatedPoint.x(), interpolatedPoint.y(), interpolatedPoint.z() + mExtrusionHeight ) ) ) );
            transformedParts.append( QgsGeometry( new QgsLineString( QgsPoint( distance, interpolatedPoint.z() ),
                                                  QgsPoint( distance, interpolatedPoint.z() + mExtrusionHeight ) ) ) );
            mResults->minZ = std::min( mResults->minZ, interpolatedPoint.z() + mExtrusionHeight );
            mResults->maxZ = std::max( mResults->maxZ, interpolatedPoint.z() + mExtrusionHeight );
          }
          else
          {
            mResults->geometries.append( QgsGeometry( new QgsPoint( interpolatedPoint ) ) );
            transformedParts.append( QgsGeometry( new QgsPoint( distance, interpolatedPoint.z() ) ) );
          }
        }
        break;
      case QgsWkbTypes::LineGeometry:
        if ( const QgsMultiLineString *ml = qgsgeometry_cast< const QgsMultiLineString * >( intersect ) )
        {
          const int numLines = ml->numGeometries();
          for ( int i = 0; i < numLines; ++i )
          {
            processTriangleLineIntersect( triangle, ml->geometryN( i ), transformedParts );
          }
        }
        else if ( const QgsLineString *ls = qgsgeometry_cast< const QgsLineString * >( intersect ) )
        {
          const int numPoints = ls->numPoints();
          QVector< double > newX;
          newX.resize( numPoints );
          QVector< double > newY;
          newY.resize( numPoints );
          QVector< double > newZ;
          newZ.resize( numPoints );
          QVector< double > newDistance;
          newDistance.resize( numPoints );

          const double *inX = ls->xData();
          const double *inY = ls->yData();
          double *outX = newX.data();
          double *outY = newY.data();
          double *outZ = newZ.data();
          double *outDistance = newDistance.data();

          QVector< double > extrudedZ;
          double *extZOut = nullptr;
          if ( mExtrusionEnabled )
          {
            extrudedZ.resize( numPoints );
            extZOut = extrudedZ.data();
          }

          QString lastError;
          for ( int i = 0 ; i < numPoints; ++i )
          {
            double x = *inX++;
            double y = *inY++;

            QgsPoint interpolatedPoint = interpolatePointOnTriangle( triangle, x, y );
            *outX++ = x;
            *outY++ = y;
            *outZ++ = interpolatedPoint.z();
            if ( extZOut )
              *extZOut++ = interpolatedPoint.z() + mExtrusionHeight;

            mResults->rawPoints.append( interpolatedPoint );
            mResults->minZ = std::min( mResults->minZ, interpolatedPoint.z() );
            mResults->maxZ = std::max( mResults->maxZ, interpolatedPoint.z() );
            if ( mExtrusionEnabled )
            {
              mResults->minZ = std::min( mResults->minZ, interpolatedPoint.z() + mExtrusionHeight );
              mResults->maxZ = std::max( mResults->maxZ, interpolatedPoint.z() + mExtrusionHeight );
            }

            const double distance = mProfileCurveEngine->lineLocatePoint( interpolatedPoint, &lastError );
            *outDistance++ = distance;

            mResults->mDistanceToHeightMap.insert( distance, interpolatedPoint.z() );
          }

          if ( mExtrusionEnabled )
          {
            std::unique_ptr< QgsLineString > ring = std::make_unique< QgsLineString >( newX, newY, newZ );
            std::unique_ptr< QgsLineString > extrudedRing = std::make_unique< QgsLineString >( newX, newY, extrudedZ );
            std::unique_ptr< QgsLineString > reversedExtrusion( extrudedRing->reversed() );
            ring->append( reversedExtrusion.get() );
            ring->close();
            mResults->geometries.append( QgsGeometry( new QgsPolygon( ring.release() ) ) );


            std::unique_ptr< QgsLineString > distanceVHeightRing = std::make_unique< QgsLineString >( newDistance, newZ );
            std::unique_ptr< QgsLineString > extrudedDistanceVHeightRing = std::make_unique< QgsLineString >( newDistance, extrudedZ );
            std::unique_ptr< QgsLineString > reversedDistanceVHeightExtrusion( extrudedDistanceVHeightRing->reversed() );
            distanceVHeightRing->append( reversedDistanceVHeightExtrusion.get() );
            distanceVHeightRing->close();
            transformedParts.append( QgsGeometry( new QgsPolygon( distanceVHeightRing.release() ) ) );
          }
          else
          {
            mResults->geometries.append( QgsGeometry( new QgsLineString( newX, newY, newZ ) ) );
            transformedParts.append( QgsGeometry( new QgsLineString( newDistance, newZ ) ) );
          }
        }
        break;

      case QgsWkbTypes::PolygonGeometry:
      case QgsWkbTypes::UnknownGeometry:
      case QgsWkbTypes::NullGeometry:
        return;
    }
  };

  auto processPolygon = [this, &processTriangleLineIntersect]( const QgsCurvePolygon * polygon, QVector< QgsGeometry > &transformedParts )
  {
    std::unique_ptr< QgsPolygon > clampedPolygon;
    if ( const QgsPolygon *p = qgsgeometry_cast< const QgsPolygon * >( polygon ) )
    {
      clampedPolygon.reset( p->clone() );
    }
    else
    {
      clampedPolygon.reset( qgsgeometry_cast< QgsPolygon * >( p->segmentize() ) );
    }
    clampAltitudes( clampedPolygon.get() );

    if ( mFeedback->isCanceled() )
      return;

    const QgsRectangle bounds = clampedPolygon->boundingBox();
    QgsTessellator t( bounds, false, false, false, false );
    t.addPolygon( *clampedPolygon, 0 );

    QgsGeometry tessellation( t.asMultiPolygon() );
    if ( mFeedback->isCanceled() )
      return;

    tessellation.translate( bounds.xMinimum(), bounds.yMinimum() );

    // iterate through the tessellation, finding triangles which intersect the line
    const int numTriangles = qgsgeometry_cast< const QgsMultiPolygon * >( tessellation.constGet() )->numGeometries();
    for ( int i = 0; i < numTriangles; ++i )
    {
      if ( mFeedback->isCanceled() )
        return;

      const QgsPolygon *triangle = qgsgeometry_cast< const QgsPolygon * >( qgsgeometry_cast< const QgsMultiPolygon * >( tessellation.constGet() )->geometryN( i ) );
      if ( !mProfileCurveEngine->intersects( triangle ) )
        continue;

      QString error;
      std::unique_ptr< QgsAbstractGeometry > intersection( mProfileCurveEngine->intersection( triangle, &error ) );
      if ( !intersection )
        continue;

      processTriangleLineIntersect( triangle, intersection.get(), transformedParts );
    }
  };

  QgsFeature feature;
  QgsFeatureIterator it = mSource->getFeatures( request );
  while ( it.nextFeature( feature ) )
  {
    if ( mFeedback->isCanceled() )
      return false;

    if ( !mProfileCurveEngine->intersects( feature.geometry().constGet() ) )
      continue;

    const QgsGeometry g = feature.geometry();
    QVector< QgsGeometry > transformedParts;
    if ( g.isMultipart() )
    {
      for ( auto it = g.const_parts_begin(); it != g.const_parts_end(); ++it )
      {
        if ( mFeedback->isCanceled() )
          break;

        if ( !mProfileCurveEngine->intersects( *it ) )
          continue;

        processPolygon( qgsgeometry_cast< const QgsCurvePolygon * >( *it ), transformedParts );
      }
    }
    else
    {
      processPolygon( qgsgeometry_cast< const QgsCurvePolygon * >( g.constGet() ), transformedParts );
    }

    if ( mFeedback->isCanceled() )
      return false;

    if ( !transformedParts.empty() )
    {
      QgsGeometry unioned = QgsGeometry::unaryUnion( transformedParts );
      if ( unioned.type() == QgsWkbTypes::LineGeometry )
        unioned = unioned.mergeLines();
      mResults->distanceVHeightGeometries.append( unioned );
    }
  }
  return true;
}

double QgsVectorLayerProfileGenerator::terrainHeight( double x, double y )
{
  if ( !mTerrainProvider )
    return std::numeric_limits<double>::quiet_NaN();

  // transform feature point to terrain provider crs
  try
  {
    double dummyZ = 0;
    mTargetToTerrainProviderTransform.transformInPlace( x, y, dummyZ );
  }
  catch ( QgsCsException & )
  {
    return std::numeric_limits<double>::quiet_NaN();
  }

  return mTerrainProvider->heightAt( x, y );
}

double QgsVectorLayerProfileGenerator::featureZToHeight( double x, double y, double z )
{
  switch ( mClamping )
  {
    case Qgis::AltitudeClamping::Absolute:
      break;

    case Qgis::AltitudeClamping::Relative:
    case Qgis::AltitudeClamping::Terrain:
    {
      const double terrainZ = terrainHeight( x, y );
      if ( !std::isnan( terrainZ ) )
      {
        switch ( mClamping )
        {
          case Qgis::AltitudeClamping::Relative:
            if ( std::isnan( z ) )
              z = terrainZ;
            else
              z += terrainZ;
            break;

          case Qgis::AltitudeClamping::Terrain:
            z = terrainZ;
            break;

          case Qgis::AltitudeClamping::Absolute:
            break;
        }
      }
      break;
    }
  }

  return z * mScale + mOffset;
}

void QgsVectorLayerProfileGenerator::clampAltitudes( QgsLineString *lineString, const QgsPoint &centroid )
{
  for ( int i = 0; i < lineString->nCoordinates(); ++i )
  {
    if ( mFeedback->isCanceled() )
      break;

    double terrainZ = 0;
    switch ( mClamping )
    {
      case Qgis::AltitudeClamping::Relative:
      case Qgis::AltitudeClamping::Terrain:
      {
        QgsPointXY pt;
        switch ( mBinding )
        {
          case Qgis::AltitudeBinding::Vertex:
            pt.setX( lineString->xAt( i ) );
            pt.setY( lineString->yAt( i ) );
            break;

          case Qgis::AltitudeBinding::Centroid:
            pt.set( centroid.x(), centroid.y() );
            break;
        }

        terrainZ = terrainHeight( pt.x(), pt.y() );
        break;
      }

      case Qgis::AltitudeClamping::Absolute:
        break;
    }

    double geomZ = 0;

    switch ( mClamping )
    {
      case Qgis::AltitudeClamping::Absolute:
      case Qgis::AltitudeClamping::Relative:
        geomZ = lineString->zAt( i );
        break;

      case Qgis::AltitudeClamping::Terrain:
        break;
    }

    const double z = ( terrainZ + geomZ ) * mScale + mOffset;
    lineString->setZAt( i, z );
  }
}

bool QgsVectorLayerProfileGenerator::clampAltitudes( QgsPolygon *polygon )
{
  if ( !polygon->is3D() )
    polygon->addZValue( 0 );

  QgsPoint centroid;
  switch ( mBinding )
  {
    case Qgis::AltitudeBinding::Vertex:
      break;

    case Qgis::AltitudeBinding::Centroid:
      centroid = polygon->centroid();
      break;
  }

  QgsCurve *curve = const_cast<QgsCurve *>( polygon->exteriorRing() );
  QgsLineString *lineString = qgsgeometry_cast<QgsLineString *>( curve );
  if ( !lineString )
    return false;

  clampAltitudes( lineString, centroid );

  for ( int i = 0; i < polygon->numInteriorRings(); ++i )
  {
    if ( mFeedback->isCanceled() )
      break;

    QgsCurve *curve = const_cast<QgsCurve *>( polygon->interiorRing( i ) );
    QgsLineString *lineString = qgsgeometry_cast<QgsLineString *>( curve );
    if ( !lineString )
      return false;

    clampAltitudes( lineString, centroid );
  }
  return true;
}

