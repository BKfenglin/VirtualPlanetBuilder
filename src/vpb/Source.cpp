/* -*-c++-*- OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield 
 *
 * This library is open source and may be redistributed and/or modified under  
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or 
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * OpenSceneGraph Public License for more details.
*/


#include <vpb/Source>
#include <vpb/Destination>
#include <vpb/DataSet>

#include <osg/Notify>
#include <osg/io_utils>
#include <osgDB/ReadFile>
#include <osgDB/FileNameUtils>

#include <gdal_priv.h>
#include <gdalwarper.h>

using namespace vpb;

SourceData::~SourceData()
{
    if (_gdalDataset) GDALClose(_gdalDataset);
}

float SourceData::getInterpolatedValue(osg::HeightField* hf, double x, double y)
{
    double r, c;
    c = (x - hf->getOrigin().x()) / hf->getXInterval();
    r = (y - hf->getOrigin().y()) / hf->getYInterval();

    int rowMin = osg::maximum((int)floor(r), 0);
    int rowMax = osg::maximum(osg::minimum((int)ceil(r), (int)(hf->getNumRows()-1)), 0);
    int colMin = osg::maximum((int)floor(c), 0);
    int colMax = osg::maximum(osg::minimum((int)ceil(c), (int)(hf->getNumColumns()-1)), 0);

    if (rowMin > rowMax) rowMin = rowMax;
    if (colMin > colMax) colMin = colMax;

    float urHeight = hf->getHeight(colMax, rowMax);
    float llHeight = hf->getHeight(colMin, rowMin);
    float ulHeight = hf->getHeight(colMin, rowMax);
    float lrHeight = hf->getHeight(colMax, rowMin);

    double x_rem = c - (int)c;
    double y_rem = r - (int)r;

    double w00 = (1.0 - y_rem) * (1.0 - x_rem) * (double)llHeight;
    double w01 = (1.0 - y_rem) * x_rem * (double)lrHeight;
    double w10 = y_rem * (1.0 - x_rem) * (double)ulHeight;
    double w11 = y_rem * x_rem * (double)urHeight;

    float result = (float)(w00 + w01 + w10 + w11);

    return result;
}

float SourceData::getInterpolatedValue(GDALRasterBand *band, double x, double y)
{
    double geoTransform[6];
    geoTransform[0] = _geoTransform(3,0);
    geoTransform[1] = _geoTransform(0,0);
    geoTransform[2] = _geoTransform(1,0);
    geoTransform[3] = _geoTransform(3,1);
    geoTransform[4] = _geoTransform(0,1);
    geoTransform[5] = _geoTransform(1,1);

    // shift the transform to the middle of the cell if a raster format is used
#ifdef SHIFT_RASTER_BY_HALF_CELL
    if (_dataType == RASTER)
    {
        geoTransform[0] += 0.5 * geoTransform[1];
        geoTransform[3] += 0.5 * geoTransform[5];
    }
#endif

    double invTransform[6];
    GDALInvGeoTransform(geoTransform, invTransform);
    double r, c;
    GDALApplyGeoTransform(invTransform, x, y, &c, &r);
   
    int rowMin = osg::maximum((int)floor(r), 0);
    int rowMax = osg::maximum(osg::minimum((int)ceil(r), (int)(_numValuesY-1)), 0);
    int colMin = osg::maximum((int)floor(c), 0);
    int colMax = osg::maximum(osg::minimum((int)ceil(c), (int)(_numValuesX-1)), 0);

    if (rowMin > rowMax) rowMin = rowMax;
    if (colMin > colMax) colMin = colMax;

    float urHeight, llHeight, ulHeight, lrHeight;

    band->RasterIO(GF_Read, colMin, rowMin, 1, 1, &llHeight, 1, 1, GDT_Float32, 0, 0);
    band->RasterIO(GF_Read, colMin, rowMax, 1, 1, &ulHeight, 1, 1, GDT_Float32, 0, 0);
    band->RasterIO(GF_Read, colMax, rowMin, 1, 1, &lrHeight, 1, 1, GDT_Float32, 0, 0);
    band->RasterIO(GF_Read, colMax, rowMax, 1, 1, &urHeight, 1, 1, GDT_Float32, 0, 0);

    int success = 0;
    float noDataValue = band->GetNoDataValue(&success);
    if (success)
    {
      if (llHeight == noDataValue) llHeight = 0.0f;
      if (ulHeight == noDataValue) ulHeight = 0.0f;
      if (lrHeight == noDataValue) lrHeight = 0.0f;
      if (urHeight == noDataValue) urHeight = 0.0f;
    }

    double x_rem = c - (int)c;
    double y_rem = r - (int)r;

    double w00 = (1.0 - y_rem) * (1.0 - x_rem) * (double)llHeight;
    double w01 = (1.0 - y_rem) * x_rem * (double)lrHeight;
    double w10 = y_rem * (1.0 - x_rem) * (double)ulHeight;
    double w11 = y_rem * x_rem * (double)urHeight;

    float result = (float)(w00 + w01 + w10 + w11);

    return result;
}

SourceData* SourceData::readData(Source* source)
{
    if (!source) return 0;


    switch(source->getType())
    {
    case(Source::IMAGE):
    case(Source::HEIGHT_FIELD):
        {
            // try osgDB for source if height data is a vector data set
            if ((source->getType() == Source::HEIGHT_FIELD) &&
                (source->_dataType == Source::VECTOR))
            {
                osg::HeightField* hf = (osg::HeightField*)source->getHFDataset();
                if (!hf)
                    hf = osgDB::readHeightFieldFile(source->getFileName().c_str());
                    
                if (hf)
                {
                    osg::notify(osg::INFO)<<"readData: HeightField recognised: "<<source->getFileName().c_str()<<std::endl;
                    SourceData* data = new SourceData(source);

                    // need to set vector or raster
                    data->_dataType = source->_dataType;

                    data->_hfDataset = hf;

                    data->_numValuesX = hf->getNumColumns();
                    data->_numValuesY = hf->getNumRows();
                    data->_numValuesZ = 1;
                    data->_hasGCPs = false;
                    data->_cs = new osg::CoordinateSystemNode("WKT","");

                    double geoTransform[6];
                    for (int i=0 ; i < 6 ; i++)
                        geoTransform[i] = 0.0;
                    // top left vertex - represent as top down
                    osg::Vec3 vertex = hf->getVertex(0,hf->getNumRows()-1);
                    geoTransform[0] = vertex.x();
                    geoTransform[3] = vertex.y();
                    geoTransform[1] = hf->getXInterval();
                    geoTransform[5] = -hf->getYInterval();
                    data->_geoTransform.set( geoTransform[1],    geoTransform[4],    0.0,    0.0,
                                             geoTransform[2],    geoTransform[5],    0.0,    0.0,
                                             0.0,                0.0,                1.0,    0.0,
                                             geoTransform[0],    geoTransform[3],    0.0,    1.0);
                                            
                    data->computeExtents();
                    return data;
                }
            }

            GDALDataset* gdalDataSet = source->getGdalDataset();
            if(!gdalDataSet)
                gdalDataSet = (GDALDataset*)GDALOpen(source->getFileName().c_str(),GA_ReadOnly);
            if (gdalDataSet)
            {
                SourceData* data = new SourceData(source);

                // need to set vector or raster
                data->_dataType = source->_dataType;

                data->_gdalDataset = gdalDataSet;
                
                data->_numValuesX = gdalDataSet->GetRasterXSize();
                data->_numValuesY = gdalDataSet->GetRasterYSize();
                data->_numValuesZ = gdalDataSet->GetRasterCount();
                data->_hasGCPs = gdalDataSet->GetGCPCount()!=0;

                const char* pszSourceSRS = gdalDataSet->GetProjectionRef();
                if (!pszSourceSRS || strlen(pszSourceSRS)==0) pszSourceSRS = gdalDataSet->GetGCPProjection();
                
                data->_cs = new osg::CoordinateSystemNode("WKT",pszSourceSRS);

                double geoTransform[6];
                if (gdalDataSet->GetGeoTransform(geoTransform)==CE_None)
                {
#ifdef SHIFT_RASTER_BY_HALF_CELL
                    // shift the transform to the middle of the cell if a raster interpreted as vector
                    if (data->_dataType == VECTOR)
                    {
                        geoTransform[0] += 0.5 * geoTransform[1];
                        geoTransform[3] += 0.5 * geoTransform[5];
                    }
#endif
                    data->_geoTransform.set( geoTransform[1],    geoTransform[4],    0.0,    0.0,
                                             geoTransform[2],    geoTransform[5],    0.0,    0.0,
                                             0.0,                0.0,                1.0,    0.0,
                                             geoTransform[0],    geoTransform[3],    0.0,    1.0);
                                            
                    data->computeExtents();

                }
                else if (gdalDataSet->GetGCPCount()>0 && gdalDataSet->GetGCPProjection())
                {
                    osg::notify(osg::INFO) << "    Using GCP's"<< std::endl;


                    /* -------------------------------------------------------------------- */
                    /*      Create a transformation object from the source to               */
                    /*      destination coordinate system.                                  */
                    /* -------------------------------------------------------------------- */
                    void *hTransformArg = 
                        GDALCreateGenImgProjTransformer( gdalDataSet, pszSourceSRS, 
                                                         NULL, pszSourceSRS, 
                                                         TRUE, 0.0, 1 );

                    if ( hTransformArg == NULL )
                    {
                        osg::notify(osg::INFO)<<" failed to create transformer"<<std::endl;
                        return NULL;
                    }

                    /* -------------------------------------------------------------------- */
                    /*      Get approximate output definition.                              */
                    /* -------------------------------------------------------------------- */
                    double adfDstGeoTransform[6];
                    int nPixels=0, nLines=0;
                    if( GDALSuggestedWarpOutput( gdalDataSet, 
                                                 GDALGenImgProjTransform, hTransformArg, 
                                                 adfDstGeoTransform, &nPixels, &nLines )
                        != CE_None )
                    {
                        osg::notify(osg::INFO)<<" failed to create warp"<<std::endl;
                        return NULL;
                    }

                    GDALDestroyGenImgProjTransformer( hTransformArg );


                    data->_geoTransform.set( adfDstGeoTransform[1],    adfDstGeoTransform[4],    0.0,    0.0,
                                             adfDstGeoTransform[2],    adfDstGeoTransform[5],    0.0,    0.0,
                                             0.0,                0.0,                1.0,    0.0,
                                             adfDstGeoTransform[0],    adfDstGeoTransform[3],    0.0,    1.0);

                    data->computeExtents();
                    
                }
                else
                {
                    osg::notify(osg::INFO) << "    No GeoTransform or GCP's - unable to compute position in space"<< std::endl;
                    
                    data->_geoTransform.set( 1.0,    0.0,    0.0,    0.0,
                                             0.0,    1.0,    0.0,    0.0,
                                             0.0,    0.0,    1.0,    0.0,
                                             0.0,    0.0,    0.0,    1.0);
                                            
                    data->computeExtents();

                }
                return data;
            }                
        }
    case(Source::MODEL):
        {
            osg::Node* model = osgDB::readNodeFile(source->getFileName().c_str());
            if (model)
            {
                SourceData* data = new SourceData(source);
                data->_model = model;
                data->_extents.expandBy(model->getBound());
            }
            
        }
        break;
    }
    
    return 0;
}

GeospatialExtents SourceData::getExtents(const osg::CoordinateSystemNode* cs) const
{
    return computeSpatialProperties(cs)._extents;
}

const SpatialProperties& SourceData::computeSpatialProperties(const osg::CoordinateSystemNode* cs) const
{
    // check to see it exists in the _spatialPropertiesMap first.
    SpatialPropertiesMap::const_iterator itr = _spatialPropertiesMap.find(cs);
    if (itr!=_spatialPropertiesMap.end()) 
    {
        return itr->second;
    }

    if (areCoordinateSystemEquivalent(_cs.get(),cs))
    {
        return *this;
    }

    if (_cs.valid() && cs)
    {
        
        if (_gdalDataset)
        {

            //osg::notify(osg::INFO)<<"Projecting bounding volume for "<<_source->getFileName()<<std::endl;

            
            // insert into the _spatialPropertiesMap for future reuse.
            _spatialPropertiesMap[cs] = *this;
            SpatialProperties& sp = _spatialPropertiesMap[cs];
            
            /* -------------------------------------------------------------------- */
            /*      Create a transformation object from the source to               */
            /*      destination coordinate system.                                  */
            /* -------------------------------------------------------------------- */
            void *hTransformArg = 
                GDALCreateGenImgProjTransformer( _gdalDataset,_cs->getCoordinateSystem().c_str(),
                                                 NULL, cs->getCoordinateSystem().c_str(),
                                                 TRUE, 0.0, 1 );

            if (!hTransformArg)
            {
                osg::notify(osg::INFO)<<" failed to create transformer"<<std::endl;
                return sp;
            }
        
            double adfDstGeoTransform[6];
            int nPixels=0, nLines=0;
            if( GDALSuggestedWarpOutput( _gdalDataset, 
                                         GDALGenImgProjTransform, hTransformArg, 
                                         adfDstGeoTransform, &nPixels, &nLines )
                != CE_None )
            {
                osg::notify(osg::INFO)<<" failed to create warp"<<std::endl;
                return sp;
            }

            sp._numValuesX = nPixels;
            sp._numValuesY = nLines;
            sp._cs = const_cast<osg::CoordinateSystemNode*>(cs);
            sp._geoTransform.set( adfDstGeoTransform[1],    adfDstGeoTransform[4],  0.0,    0.0,
                                  adfDstGeoTransform[2],    adfDstGeoTransform[5],  0.0,    0.0,
                                  0.0,                      0.0,                    1.0,    0.0,
                                  adfDstGeoTransform[0],    adfDstGeoTransform[3],  0.0,    1.0);

            GDALDestroyGenImgProjTransformer( hTransformArg );

            sp.computeExtents();

            return sp;
        }

    }
    osg::notify(osg::INFO)<<"DataSet::DataSource::assuming compatible coordinates."<<std::endl;
    return *this;
}

bool SourceData::intersects(const SpatialProperties& sp) const
{
    return sp._extents.intersects(getExtents(sp._cs.get()));
}

void SourceData::read(DestinationData& destination)
{
    osg::notify(osg::INFO)<<"A"<<std::endl;

    if (!_source) return;
    
    osg::notify(osg::INFO)<<"B"<<std::endl;

    switch (_source->getType())
    {
    case(Source::IMAGE):
        osg::notify(osg::INFO)<<"B.1"<<std::endl;
        readImage(destination);
        break;
    case(Source::HEIGHT_FIELD):
        osg::notify(osg::INFO)<<"B.2"<<std::endl;
        readHeightField(destination);
        break;
    case(Source::MODEL):
        osg::notify(osg::INFO)<<"B.3"<<std::endl;
        readModels(destination);
        break;
    }
    osg::notify(osg::INFO)<<"C"<<std::endl;
}

void SourceData::readImage(DestinationData& destination)
{
    osg::notify(osg::INFO)<<std::endl<<"readImage "<<std::endl;

    if (destination._image.valid())
    {
        
        GeospatialExtents s_bb = getExtents(destination._cs.get());
        GeospatialExtents d_bb = destination._extents;

        // note, we have to handle the possibility of goegraphic datasets wrapping over on themselves when they pass over the dateline
        // to do this we have to test geographic datasets via two passes, each with a 360 degree shift of the source cata.
        double xoffset = ((d_bb.xMin() < s_bb.xMin()) && (d_bb._isGeographic)) ? -360.0 : 0.0;
        unsigned int numXChecks = d_bb._isGeographic ? 2 : 1;
        for(unsigned int ic = 0; ic < numXChecks; ++ic, xoffset += 360.0)
        {
        
            osg::notify(osg::INFO)<<"Testing "<<xoffset<<std::endl;
            osg::notify(osg::INFO)<<"  s_bb "<<s_bb.xMin()+xoffset<<" "<<s_bb.xMax()+xoffset<<std::endl;
            osg::notify(osg::INFO)<<"  d_bb "<<d_bb.xMin()<<" "<<d_bb.xMax()<<std::endl;
        
            GeospatialExtents intersect_bb(d_bb.intersection(s_bb, xoffset));
            if (!intersect_bb.valid())
            {   
                osg::notify(osg::INFO)<<"Reading image but it does not intesection destination - ignoring"<<std::endl;
                continue;
            }

            osg::notify(osg::INFO)<<"readImage s_bb is geographic "<<s_bb._isGeographic<<std::endl;
            osg::notify(osg::INFO)<<"readImage d_bb is geographic "<<d_bb._isGeographic<<std::endl;
            osg::notify(osg::INFO)<<"readImage intersect_bb is geographic "<<intersect_bb._isGeographic<<std::endl;



           int windowX = osg::maximum((int)floorf((float)_numValuesX*(intersect_bb.xMin()-xoffset-s_bb.xMin())/(s_bb.xMax()-s_bb.xMin())),0);
           int windowY = osg::maximum((int)floorf((float)_numValuesY*(intersect_bb.yMin()-s_bb.yMin())/(s_bb.yMax()-s_bb.yMin())),0);
           int windowWidth = osg::minimum((int)ceilf((float)_numValuesX*(intersect_bb.xMax()-xoffset-s_bb.xMin())/(s_bb.xMax()-s_bb.xMin())),(int)_numValuesX)-windowX;
           int windowHeight = osg::minimum((int)ceilf((float)_numValuesY*(intersect_bb.yMax()-s_bb.yMin())/(s_bb.yMax()-s_bb.yMin())),(int)_numValuesY)-windowY;

           int destX = osg::maximum((int)floorf((float)destination._image->s()*(intersect_bb.xMin()-d_bb.xMin())/(d_bb.xMax()-d_bb.xMin())),0);
           int destY = osg::maximum((int)floorf((float)destination._image->t()*(intersect_bb.yMin()-d_bb.yMin())/(d_bb.yMax()-d_bb.yMin())),0);
           int destWidth = osg::minimum((int)ceilf((float)destination._image->s()*(intersect_bb.xMax()-d_bb.xMin())/(d_bb.xMax()-d_bb.xMin())),(int)destination._image->s())-destX;
           int destHeight = osg::minimum((int)ceilf((float)destination._image->t()*(intersect_bb.yMax()-d_bb.yMin())/(d_bb.yMax()-d_bb.yMin())),(int)destination._image->t())-destY;

            osg::notify(osg::INFO)<<"   copying from "<<windowX<<"\t"<<windowY<<"\t"<<windowWidth<<"\t"<<windowHeight<<std::endl;
            osg::notify(osg::INFO)<<"             to "<<destX<<"\t"<<destY<<"\t"<<destWidth<<"\t"<<destHeight<<std::endl;

            int readWidth = destWidth;
            int readHeight = destHeight;
            bool doResample = false;

            float destWindowWidthRatio = (float)destWidth/(float)windowWidth;
            float destWindowHeightRatio = (float)destHeight/(float)windowHeight;
            const float resizeTolerance = 1.1;

            bool interpolateSourceImagery = true;
            if (interpolateSourceImagery && 
                (destWindowWidthRatio>resizeTolerance || destWindowHeightRatio>resizeTolerance) &&
                windowWidth>=2 && windowHeight>=2)
            {
                readWidth = windowWidth;
                readHeight = windowHeight;
                doResample = true;
            }

            bool hasRGB = _gdalDataset->GetRasterCount() >= 3;
            bool hasAlpha = _gdalDataset->GetRasterCount() >= 4;
            bool hasColorTable = _gdalDataset->GetRasterCount() >= 1 && _gdalDataset->GetRasterBand(1)->GetColorTable();
            bool hasGreyScale = _gdalDataset->GetRasterCount() == 1;
            unsigned int numSourceComponents = hasAlpha?4:3;

            if (hasRGB || hasColorTable || hasGreyScale)
            {
                // RGB

                unsigned int numBytesPerPixel = 1;
                GDALDataType targetGDALType = GDT_Byte;

                int pixelSpace=numSourceComponents*numBytesPerPixel;

                osg::notify(osg::INFO) << "reading RGB"<<std::endl;

                unsigned char* tempImage = new unsigned char[readWidth*readHeight*pixelSpace];


                /* New code courtesy of Frank Warmerdam of the GDAL group */

                // RGB images ... or at least we assume 3+ band images can be treated 
                // as RGB. 
                if( hasRGB ) 
                { 
                    GDALRasterBand* bandRed = _gdalDataset->GetRasterBand(1); 
                    GDALRasterBand* bandGreen = _gdalDataset->GetRasterBand(2); 
                    GDALRasterBand* bandBlue = _gdalDataset->GetRasterBand(3); 
                    GDALRasterBand* bandAlpha = hasAlpha ? _gdalDataset->GetRasterBand(4) : 0; 

                    bandRed->RasterIO(GF_Read, 
                                      windowX,_numValuesY-(windowY+windowHeight), 
                                      windowWidth,windowHeight, 
                                      (void*)(tempImage+0),readWidth,readHeight, 
                                      targetGDALType,pixelSpace,pixelSpace*readWidth); 
                    bandGreen->RasterIO(GF_Read, 
                                        windowX,_numValuesY-(windowY+windowHeight), 
                                        windowWidth,windowHeight, 
                                        (void*)(tempImage+1),readWidth,readHeight, 
                                        targetGDALType,pixelSpace,pixelSpace*readWidth); 
                    bandBlue->RasterIO(GF_Read, 
                                       windowX,_numValuesY-(windowY+windowHeight), 
                                       windowWidth,windowHeight, 
                                       (void*)(tempImage+2),readWidth,readHeight, 
                                       targetGDALType,pixelSpace,pixelSpace*readWidth); 

                    if (bandAlpha)
                    {
                        bandAlpha->RasterIO(GF_Read, 
                                           windowX,_numValuesY-(windowY+windowHeight), 
                                           windowWidth,windowHeight, 
                                           (void*)(tempImage+3),readWidth,readHeight, 
                                           targetGDALType,pixelSpace,pixelSpace*readWidth); 
                    }
                } 

                else if( hasColorTable ) 
                { 
                    // Pseudocolored image.  Convert 1 band + color table to 24bit RGB. 

                    GDALRasterBand *band; 
                    GDALColorTable *ct; 
                    int i; 


                    band = _gdalDataset->GetRasterBand(1); 


                    band->RasterIO(GF_Read, 
                                   windowX,_numValuesY-(windowY+windowHeight), 
                                   windowWidth,windowHeight, 
                                   (void*)(tempImage+0),readWidth,readHeight, 
                                   targetGDALType,pixelSpace,pixelSpace*readWidth); 


                    ct = band->GetColorTable(); 


                    for( i = 0; i < readWidth * readHeight; i++ ) 
                    { 
                        GDALColorEntry sEntry; 


                        // default to greyscale equilvelent. 
                        sEntry.c1 = tempImage[i*3]; 
                        sEntry.c2 = tempImage[i*3]; 
                        sEntry.c3 = tempImage[i*3]; 


                        ct->GetColorEntryAsRGB( tempImage[i*3], &sEntry ); 


                        // Apply RGB back over destination image. 
                        tempImage[i*3 + 0] = sEntry.c1; 
                        tempImage[i*3 + 1] = sEntry.c2; 
                        tempImage[i*3 + 2] = sEntry.c3; 
                    } 
                } 


                else if (hasGreyScale)
                { 
                    // Greyscale image.  Convert 1 band to 24bit RGB. 
                    GDALRasterBand *band; 


                    band = _gdalDataset->GetRasterBand(1); 


                    band->RasterIO(GF_Read, 
                                   windowX,_numValuesY-(windowY+windowHeight), 
                                   windowWidth,windowHeight, 
                                   (void*)(tempImage+0),readWidth,readHeight, 
                                   targetGDALType,pixelSpace,pixelSpace*readWidth); 
                    band->RasterIO(GF_Read, 
                                   windowX,_numValuesY-(windowY+windowHeight), 
                                   windowWidth,windowHeight, 
                                   (void*)(tempImage+1),readWidth,readHeight, 
                                   targetGDALType,pixelSpace,pixelSpace*readWidth); 
                    band->RasterIO(GF_Read, 
                                   windowX,_numValuesY-(windowY+windowHeight), 
                                   windowWidth,windowHeight, 
                                   (void*)(tempImage+2),readWidth,readHeight, 
                                   targetGDALType,pixelSpace,pixelSpace*readWidth); 
                } 

                if (doResample || readWidth!=destWidth || readHeight!=destHeight)
                {
                    unsigned char* destImage = new unsigned char[destWidth*destHeight*pixelSpace];

                    // rescale image by hand as glu seem buggy....
                    for(int j=0;j<destHeight;++j)
                    {
                        float  t_d = (destHeight>1)?((float)j/((float)destHeight-1)):0;
                        for(int i=0;i<destWidth;++i)
                        {
                            float s_d = (destWidth>1)?((float)i/((float)destWidth-1)):0;

                            float flt_read_i = s_d * ((float)readWidth-1);
                            float flt_read_j = t_d * ((float)readHeight-1);

                            int read_i = (int)flt_read_i;
                            if (read_i>=readWidth) read_i=readWidth-1;

                            float flt_read_ir = flt_read_i-read_i;
                            if (read_i==readWidth-1) flt_read_ir=0.0f;

                            int read_j = (int)flt_read_j;
                            if (read_j>=readHeight) read_j=readHeight-1;

                            float flt_read_jr = flt_read_j-read_j;
                            if (read_j==readHeight-1) flt_read_jr=0.0f;

                            unsigned char* dest = destImage + (j*destWidth + i) * pixelSpace;
                            if (flt_read_ir==0.0f)  // no need to interpolate i axis.
                            {
                                if (flt_read_jr==0.0f)  // no need to interpolate j axis.
                                {
                                    // copy pixels
                                    unsigned char* src = tempImage + (read_j*readWidth + read_i) * pixelSpace;
                                    dest[0] = src[0];
                                    dest[1] = src[1];
                                    dest[2] = src[2];
                                    if (numSourceComponents==4) dest[3] = src[3];
                                    //std::cout<<"copy"<<std::endl;
                                }
                                else  // need to interpolate j axis.
                                {
                                    // copy pixels
                                    unsigned char* src_0 = tempImage + (read_j*readWidth + read_i) * pixelSpace;
                                    unsigned char* src_1 = src_0 + readWidth*pixelSpace;
                                    float r_0 = 1.0f-flt_read_jr;
                                    float r_1 = flt_read_jr;
                                    dest[0] = (unsigned char)((float)src_0[0]*r_0 + (float)src_1[0]*r_1);
                                    dest[1] = (unsigned char)((float)src_0[1]*r_0 + (float)src_1[1]*r_1);
                                    dest[2] = (unsigned char)((float)src_0[2]*r_0 + (float)src_1[2]*r_1);
                                    if (numSourceComponents==4) dest[3] = (unsigned char)((float)src_0[3]*r_0 + (float)src_1[3]*r_1);
                                    //std::cout<<"interpolate j axis"<<std::endl;
                                }
                            }
                            else // need to interpolate i axis.
                            {
                                if (flt_read_jr==0.0f) // no need to interpolate j axis.
                                {
                                    // copy pixels
                                    unsigned char* src_0 = tempImage + (read_j*readWidth + read_i) * pixelSpace;
                                    unsigned char* src_1 = src_0 + pixelSpace;
                                    float r_0 = 1.0f-flt_read_ir;
                                    float r_1 = flt_read_ir;
                                    dest[0] = (unsigned char)((float)src_0[0]*r_0 + (float)src_1[0]*r_1);
                                    dest[1] = (unsigned char)((float)src_0[1]*r_0 + (float)src_1[1]*r_1);
                                    dest[2] = (unsigned char)((float)src_0[2]*r_0 + (float)src_1[2]*r_1);
                                    if (numSourceComponents==4) dest[3] = (unsigned char)((float)src_0[3]*r_0 + (float)src_1[3]*r_1);
                                    //std::cout<<"interpolate i axis"<<std::endl;
                                }
                                else  // need to interpolate i and j axis.
                                {
                                    unsigned char* src_0 = tempImage + (read_j*readWidth + read_i) * pixelSpace;
                                    unsigned char* src_1 = src_0 + readWidth*pixelSpace;
                                    unsigned char* src_2 = src_0 + pixelSpace;
                                    unsigned char* src_3 = src_1 + pixelSpace;
                                    float r_0 = (1.0f-flt_read_ir)*(1.0f-flt_read_jr);
                                    float r_1 = (1.0f-flt_read_ir)*flt_read_jr;
                                    float r_2 = (flt_read_ir)*(1.0f-flt_read_jr);
                                    float r_3 = (flt_read_ir)*flt_read_jr;
                                    dest[0] = (unsigned char)(((float)src_0[0])*r_0 + ((float)src_1[0])*r_1 + ((float)src_2[0])*r_2 + ((float)src_3[0])*r_3);
                                    dest[1] = (unsigned char)(((float)src_0[1])*r_0 + ((float)src_1[1])*r_1 + ((float)src_2[1])*r_2 + ((float)src_3[1])*r_3);
                                    dest[2] = (unsigned char)(((float)src_0[2])*r_0 + ((float)src_1[2])*r_1 + ((float)src_2[2])*r_2 + ((float)src_3[2])*r_3);
                                    if (numSourceComponents==4) dest[3] = (unsigned char)(((float)src_0[3])*r_0 + ((float)src_1[3])*r_1 + ((float)src_2[3])*r_2 + ((float)src_3[3])*r_3);
                                    //std::cout<<"interpolate i & j axis"<<std::endl;
                                }
                            }

                        }
                    }

                    delete [] tempImage;  
                    tempImage = destImage;
                }

                // now copy into destination image
                unsigned char* sourceRowPtr = tempImage;
                int sourceRowDelta = pixelSpace*destWidth;
                unsigned char* destinationRowPtr = destination._image->data(destX,destY+destHeight-1);
                int destinationRowDelta = -(int)(destination._image->getRowSizeInBytes());
                int destination_pixelSpace = destination._image->getPixelSizeInBits()/8;
                bool destination_hasAlpha = osg::Image::computeNumComponents(destination._image->getPixelFormat())==4;

                // copy image to destination image
                for(int row=0;
                    row<destHeight;
                    ++row, sourceRowPtr+=sourceRowDelta, destinationRowPtr+=destinationRowDelta)
                {
                    unsigned char* sourceColumnPtr = sourceRowPtr;
                    unsigned char* destinationColumnPtr = destinationRowPtr;

                    for(int col=0;
                        col<destWidth;
                        ++col, sourceColumnPtr+=pixelSpace, destinationColumnPtr+=destination_pixelSpace)
                    {
                        if (hasAlpha)
                        {
                            // only copy over source pixel if its alpha value is not 0
                            if (sourceColumnPtr[3]!=0)
                            {
                                if (sourceColumnPtr[3]==255)
                                {
                                    // source alpha is full on so directly copy over.
                                    destinationColumnPtr[0] = sourceColumnPtr[0];
                                    destinationColumnPtr[1] = sourceColumnPtr[1];
                                    destinationColumnPtr[2] = sourceColumnPtr[2];

                                    if (destination_hasAlpha)
                                        destinationColumnPtr[3] = sourceColumnPtr[3];
                                }
                                else
                                {
                                    // source value isn't full on so blend it with destination 
                                    float rs = (float)sourceColumnPtr[3]/255.0f; 
                                    float rd = 1.0f-rs;

                                    destinationColumnPtr[0] = (int)(rd * (float)destinationColumnPtr[0] + rs * (float)sourceColumnPtr[0]);
                                    destinationColumnPtr[1] = (int)(rd * (float)destinationColumnPtr[1] + rs * (float)sourceColumnPtr[1]);
                                    destinationColumnPtr[2] = (int)(rd * (float)destinationColumnPtr[2] + rs * (float)sourceColumnPtr[2]);

                                    if (destination_hasAlpha)
                                        destinationColumnPtr[3] = osg::maximum(destinationColumnPtr[3],sourceColumnPtr[3]);

                                }
                            }
                        }
                        else if (sourceColumnPtr[0]!=0 || sourceColumnPtr[1]!=0 || sourceColumnPtr[2]!=0)
                        {
                            destinationColumnPtr[0] = sourceColumnPtr[0];
                            destinationColumnPtr[1] = sourceColumnPtr[1];
                            destinationColumnPtr[2] = sourceColumnPtr[2];
                        }
                    }
                }

                delete [] tempImage;

            }
            else
            {
                osg::notify(osg::INFO)<<"Warning image not read as Red, Blue and Green bands not present"<<std::endl;
            }
        }
    }
}

void SourceData::readHeightField(DestinationData& destination)
{
    osg::notify(osg::INFO)<<"\nIn SourceData::readHeightField"<<std::endl;

    if (destination._heightField.valid())
    {
        osg::notify(osg::INFO)<<"Reading height field"<<std::endl;

        GeospatialExtents s_bb = getExtents(destination._cs.get());
        GeospatialExtents d_bb = destination._extents;

        // note, we have to handle the possibility of goegraphic datasets wrapping over on themselves when they pass over the dateline
        // to do this we have to test geographic datasets via two passes, each with a 360 degree shift of the source cata.
        double xoffset = ((d_bb.xMin() < s_bb.xMin()) && (d_bb._isGeographic)) ? -360.0 : 0.0;
        unsigned int numXChecks = d_bb._isGeographic ? 2 : 1;
        for(unsigned int ic = 0; ic < numXChecks; ++ic, xoffset += 360.0)
        {
        
            osg::notify(osg::INFO)<<"Testing "<<xoffset<<std::endl;
            osg::notify(osg::INFO)<<"  s_bb "<<s_bb.xMin()+xoffset<<" "<<s_bb.xMax()+xoffset<<std::endl;
            osg::notify(osg::INFO)<<"  d_bb "<<d_bb.xMin()<<" "<<d_bb.xMax()<<std::endl;
        
            GeospatialExtents intersect_bb(d_bb.intersection(s_bb, xoffset));

            if (!intersect_bb.valid())
            {
                osg::notify(osg::INFO)<<"Reading height field but it does not intesection destination - ignoring"<<std::endl;
                continue;
            }

           int destX = osg::maximum((int)floorf((float)destination._heightField->getNumColumns()*(intersect_bb.xMin()-d_bb.xMin())/(d_bb.xMax()-d_bb.xMin())),0);
           int destY = osg::maximum((int)floorf((float)destination._heightField->getNumRows()*(intersect_bb.yMin()-d_bb.yMin())/(d_bb.yMax()-d_bb.yMin())),0);
           int destWidth = osg::minimum((int)ceilf((float)destination._heightField->getNumColumns()*(intersect_bb.xMax()-d_bb.xMin())/(d_bb.xMax()-d_bb.xMin())),(int)destination._heightField->getNumColumns())-destX;
           int destHeight = osg::minimum((int)ceilf((float)destination._heightField->getNumRows()*(intersect_bb.yMax()-d_bb.yMin())/(d_bb.yMax()-d_bb.yMin())),(int)destination._heightField->getNumRows())-destY;

            // use heightfield if it exists
            if (_hfDataset.valid())
            {
                // read the data.
                osg::HeightField* hf = destination._heightField.get();

                //float noDataValueFill = 0.0f;
                //bool ignoreNoDataValue = true;

                //Sample terrain at each vert to increase accuracy of the terrain.
                int endX = destX + destWidth;
                int endY = destY + destHeight;

                double orig_X = hf->getOrigin().x();
                double orig_Y = hf->getOrigin().y();
                double delta_X = hf->getXInterval();
                double delta_Y = hf->getYInterval();

                for (int c = destX; c < endX; ++c)
                {
                    double geoX = orig_X + (delta_X * (double)c);
                    for (int r = destY; r < endY; ++r)
                    {
                        double geoY = orig_Y + (delta_Y * (double)r);
                        float h = getInterpolatedValue(_hfDataset.get(), geoX-xoffset, geoY);
                        hf->setHeight(c,r,h);
                    }
                }
                return;
            }

            // which band do we want to read from...        
            int numBands = _gdalDataset->GetRasterCount();
            GDALRasterBand* bandGray = 0;
            GDALRasterBand* bandRed = 0;
            GDALRasterBand* bandGreen = 0;
            GDALRasterBand* bandBlue = 0;
            GDALRasterBand* bandAlpha = 0;

            for(int b=1;b<=numBands;++b)
            {
                GDALRasterBand* band = _gdalDataset->GetRasterBand(b);
                if (band->GetColorInterpretation()==GCI_GrayIndex) bandGray = band;
                else if (band->GetColorInterpretation()==GCI_RedBand) bandRed = band;
                else if (band->GetColorInterpretation()==GCI_GreenBand) bandGreen = band;
                else if (band->GetColorInterpretation()==GCI_BlueBand) bandBlue = band;
                else if (band->GetColorInterpretation()==GCI_AlphaBand) bandAlpha = band;
                else if (bandGray == 0) bandGray = band;
            }


            GDALRasterBand* bandSelected = 0;
            if (!bandSelected && bandGray) bandSelected = bandGray;
            else if (!bandSelected && bandAlpha) bandSelected = bandAlpha;
            else if (!bandSelected && bandRed) bandSelected = bandRed;
            else if (!bandSelected && bandGreen) bandSelected = bandGreen;
            else if (!bandSelected && bandBlue) bandSelected = bandBlue;

            if (bandSelected)
            {

                if (bandSelected->GetUnitType()) osg::notify(osg::INFO) << "bandSelected->GetUnitType()=" << bandSelected->GetUnitType()<<std::endl;
                else osg::notify(osg::INFO) << "bandSelected->GetUnitType()= null" <<std::endl;


                int success = 0;
                float noDataValue = bandSelected->GetNoDataValue(&success);
                if (success)
                {
                    osg::notify(osg::INFO)<<"We have NoDataValue = "<<noDataValue<<std::endl;
                }
                else
                {
                    osg::notify(osg::INFO)<<"We have no NoDataValue"<<std::endl;
                    noDataValue = 0.0f;
                }

                float offset = bandSelected->GetOffset(&success);
                if (success)
                {
                    osg::notify(osg::INFO)<<"We have Offset = "<<offset<<std::endl;
                }
                else
                {
                    osg::notify(osg::INFO)<<"We have no Offset"<<std::endl;
                    offset = 0.0f;
                }

                float scale = bandSelected->GetScale(&success);
                if (success)
                {
                    osg::notify(osg::INFO)<<"We have Scale = "<<scale<<std::endl;
                }
                else
                {
                    scale = destination._dataSet->getVerticalScale();
                    osg::notify(osg::INFO)<<"We have no Scale from file so use DataSet vertical scale of "<<scale<<std::endl;

                }

                osg::notify(osg::INFO)<<"********* getLinearUnits = "<<getLinearUnits(_cs.get())<<std::endl;

                // raad the data.
                osg::HeightField* hf = destination._heightField.get();

                float noDataValueFill = 0.0f;
                bool ignoreNoDataValue = true;

                bool interpolateTerrain = true;

                if (interpolateTerrain)
                {
                    //Sample terrain at each vert to increase accuracy of the terrain.
                    int endX = destX + destWidth;
                    int endY = destY + destHeight;

                    double orig_X = hf->getOrigin().x();
                    double orig_Y = hf->getOrigin().y();
                    double delta_X = hf->getXInterval();
                    double delta_Y = hf->getYInterval();

                    for (int c = destX; c < endX; ++c)
                    {
                        double geoX = orig_X + (delta_X * (double)c);
                        for (int r = destY; r < endY; ++r)
                        {
                            double geoY = orig_Y + (delta_Y * (double)r);
                            float h = getInterpolatedValue(bandSelected, geoX-xoffset, geoY);
                            if (h!=noDataValue) hf->setHeight(c,r,offset + h*scale);
                            else if (!ignoreNoDataValue) hf->setHeight(c,r,noDataValueFill);
                        }
                    }
                }
                else
                {
                    // compute dimensions to read from.        
                   int windowX = osg::maximum((int)floorf((float)_numValuesX*(intersect_bb.xMin()-xoffset-s_bb.xMin())/(s_bb.xMax()-s_bb.xMin())),0);
                   int windowY = osg::maximum((int)floorf((float)_numValuesY*(intersect_bb.yMin()-s_bb.yMin())/(s_bb.yMax()-s_bb.yMin())),0);
                   int windowWidth = osg::minimum((int)ceilf((float)_numValuesX*(intersect_bb.xMax()-xoffset-s_bb.xMin())/(s_bb.xMax()-s_bb.xMin())),(int)_numValuesX)-windowX;
                   int windowHeight = osg::minimum((int)ceilf((float)_numValuesY*(intersect_bb.yMax()-s_bb.yMin())/(s_bb.yMax()-s_bb.yMin())),(int)_numValuesY)-windowY;

                    osg::notify(osg::INFO)<<"   copying from "<<windowX<<"\t"<<windowY<<"\t"<<windowWidth<<"\t"<<windowHeight<<std::endl;
                    osg::notify(osg::INFO)<<"             to "<<destX<<"\t"<<destY<<"\t"<<destWidth<<"\t"<<destHeight<<std::endl;

                    // read data into temporary array
                    float* heightData = new float [ destWidth*destHeight ];

                    //bandSelected->RasterIO(GF_Read,windowX,_numValuesY-(windowY+windowHeight),windowWidth,windowHeight,floatdata,destWidth,destHeight,GDT_Float32,numBytesPerZvalue,lineSpace);
                    bandSelected->RasterIO(GF_Read,windowX,_numValuesY-(windowY+windowHeight),windowWidth,windowHeight,heightData,destWidth,destHeight,GDT_Float32,0,0);

                    float* heightPtr = heightData;

                    for(int r=destY+destHeight-1;r>=destY;--r)
                    {
                        for(int c=destX;c<destX+destWidth;++c)
                        {
                            float h = *heightPtr++;
                            if (h!=noDataValue) hf->setHeight(c,r,offset + h*scale);
                            else if (!ignoreNoDataValue) hf->setHeight(c,r,noDataValueFill);

                            h = hf->getHeight(c,r);
                        }
                    }

                    delete [] heightData;
                }          
            }
        }
    }
}

void SourceData::readModels(DestinationData& destination)
{
    if (_model.valid())
    {
        osg::notify(osg::INFO)<<"Raading model"<<std::endl;
        destination._models.push_back(_model);
    }
}

void Source::setGdalDataset(GDALDataset* gdalDataSet)
{
    _gdalDataset = gdalDataSet;
}

GDALDataset* Source::getGdalDataset()
{
    return _gdalDataset;
}

const GDALDataset* Source::getGdalDataset() const
{
    return _gdalDataset;
}

void Source::setHFDataset(osg::HeightField* hfDataSet)
{
    _hfDataset = hfDataSet;
}

osg::HeightField* Source::getHFDataset()
{
    return _hfDataset.get();
}

const osg::HeightField* Source::getHFDataset() const
{
    return _hfDataset.get();
}

void Source::setSortValueFromSourceDataResolution()
{
    if (_sourceData.valid())
    {
        double dx = (_sourceData->_extents.xMax()-_sourceData->_extents.xMin())/(double)(_sourceData->_numValuesX-1);
        double dy = (_sourceData->_extents.yMax()-_sourceData->_extents.yMin())/(double)(_sourceData->_numValuesY-1);
        
        setSortValue( sqrt( dx*dx + dy*dy ) );
    }
}

void Source::loadSourceData()
{
    osg::notify(osg::INFO)<<"Source::loadSourceData() "<<_filename<<std::endl;
    
    _sourceData = SourceData::readData(this);
    
    assignCoordinateSystemAndGeoTransformAccordingToParameterPolicy();
}    

void Source::assignCoordinateSystemAndGeoTransformAccordingToParameterPolicy()
{
    if (getCoordinateSystemPolicy()==PREFER_CONFIG_SETTINGS)
    {
        _sourceData->_cs = _cs;
        
        osg::notify(osg::INFO)<<"assigning CS from Source to Data."<<std::endl;
        
    }
    else
    {
        _cs = _sourceData->_cs;
        osg::notify(osg::INFO)<<"assigning CS from Data to Source."<<std::endl;
    }
    
    if (getGeoTransformPolicy()==PREFER_CONFIG_SETTINGS)
    {
        _sourceData->_geoTransform = _geoTransform;

        osg::notify(osg::INFO)<<"assigning GeoTransform from Source to Data."<<_geoTransform<<std::endl;

    }
    else if (getGeoTransformPolicy()==PREFER_CONFIG_SETTINGS_BUT_SCALE_BY_FILE_RESOLUTION)
    {
    
        // scale the x and y axis.
        double div_x;
        double div_y;

        // set up properly for vector and raster (previously always vector)
        if (_dataType == SpatialProperties::VECTOR)
        {
            div_x = 1.0/(double)(_sourceData->_numValuesX - 1);
            div_y = 1.0/(double)(_sourceData->_numValuesY - 1);
        }
        else    // if (_dataType == SpatialProperties::RASTER)
        {
            div_x = 1.0/(double)(_sourceData->_numValuesX);
            div_y = 1.0/(double)(_sourceData->_numValuesY);
        }
    
        _geoTransform(0,0) *= div_x;
        _geoTransform(1,0) *= div_x;
        _geoTransform(2,0) *= div_x;
    
        _geoTransform(0,1) *= div_y;
        _geoTransform(1,1) *= div_y;
        _geoTransform(2,1) *= div_y;

        _sourceData->_geoTransform = _geoTransform;

        osg::notify(osg::INFO)<<"assigning GeoTransform from Source to Data based on file resolution."<<_geoTransform<<std::endl;

    }
    else
    {
        _geoTransform = _sourceData->_geoTransform;
        osg::notify(osg::INFO)<<"assigning GeoTransform from Data to Source."<<_geoTransform<<std::endl;
    }
    
    _sourceData->computeExtents();
    
    _extents = _sourceData->_extents;
}

bool Source::needReproject(const osg::CoordinateSystemNode* cs) const
{
    return needReproject(cs,0.0,0.0);
}

bool Source::needReproject(const osg::CoordinateSystemNode* cs, double minResolution, double maxResolution) const
{
    if (!_sourceData) return false;
    
    // handle modles by using a matrix transform only.
    if (_type==MODEL) return false;
    
    // always need to reproject imagery with GCP's.
    if (_sourceData->_hasGCPs)
    {
        osg::notify(osg::INFO)<<"Need to to reproject due to presence of GCP's"<<std::endl;
        return true;
    }

    if (!areCoordinateSystemEquivalent(_cs.get(),cs))
    {
        osg::notify(osg::INFO)<<"Need to do reproject !areCoordinateSystemEquivalent(_cs.get(),cs)"<<std::endl;

        return true;
    }
     
    if (minResolution==0.0 && maxResolution==0.0) return false;

    // now check resolutions.
    const osg::Matrixd& m = _sourceData->_geoTransform;
    double currentResolution = sqrt(osg::square(m(0,0))+osg::square(m(1,0))+
                                    osg::square(m(0,1))+osg::square(m(1,1)));
                                   
    if (currentResolution<minResolution) return true;
    if (currentResolution>maxResolution) return true;

    return false;
}

Source* Source::doReproject(const std::string& filename, osg::CoordinateSystemNode* cs, double targetResolution) const
{
    // return nothing when repoject is inappropriate.
    if (!_sourceData) return 0;
    if (_type==MODEL) return 0;
    
    osg::notify(osg::NOTICE)<<"reprojecting to file "<<filename<<std::endl;

    GDALDriverH hDriver = GDALGetDriverByName( "GTiff" );
        
    if (hDriver == NULL)
    {       
    osg::notify(osg::INFO)<<"Unable to load driver for "<<"GTiff"<<std::endl;
        return 0;
    }
    
    if (GDALGetMetadataItem( hDriver, GDAL_DCAP_CREATE, NULL ) == NULL )
    {
        osg::notify(osg::INFO)<<"GDAL driver does not support create for "<<osgDB::getFileExtension(filename)<<std::endl;
        return 0;
    }

/* -------------------------------------------------------------------- */
/*      Create a transformation object from the source to               */
/*      destination coordinate system.                                  */
/* -------------------------------------------------------------------- */
    void *hTransformArg = 
         GDALCreateGenImgProjTransformer( _sourceData->_gdalDataset,_sourceData->_cs->getCoordinateSystem().c_str(),
                                          NULL, cs->getCoordinateSystem().c_str(),
                                          TRUE, 0.0, 1 );

    if (!hTransformArg)
    {
        osg::notify(osg::INFO)<<" failed to create transformer"<<std::endl;
        return 0;
    }

    double adfDstGeoTransform[6];
    int nPixels=0, nLines=0;
    if( GDALSuggestedWarpOutput( _sourceData->_gdalDataset, 
                                 GDALGenImgProjTransform, hTransformArg, 
                                 adfDstGeoTransform, &nPixels, &nLines )
        != CE_None )
    {
        osg::notify(osg::INFO)<<" failed to create warp"<<std::endl;
        return 0;
    }
    
    if (targetResolution>0.0f)
    {
        osg::notify(osg::INFO)<<"recomputing the target transform size"<<std::endl;
        
        double currentResolution = sqrt(osg::square(adfDstGeoTransform[1])+osg::square(adfDstGeoTransform[2])+
                                        osg::square(adfDstGeoTransform[4])+osg::square(adfDstGeoTransform[5]));

        osg::notify(osg::INFO)<<"        default computed resolution "<<currentResolution<<" nPixels="<<nPixels<<" nLines="<<nLines<<std::endl;

        double extentsPixels = sqrt(osg::square(adfDstGeoTransform[1])+osg::square(adfDstGeoTransform[2]))*(double)(nPixels-1);
        double extentsLines = sqrt(osg::square(adfDstGeoTransform[4])+osg::square(adfDstGeoTransform[5]))*(double)(nLines-1);
                                        
        double ratio = targetResolution/currentResolution;
        adfDstGeoTransform[1] *= ratio;
        adfDstGeoTransform[2] *= ratio;
        adfDstGeoTransform[4] *= ratio;
        adfDstGeoTransform[5] *= ratio;
        
        osg::notify(osg::INFO)<<"    extentsPixels="<<extentsPixels<<std::endl;
        osg::notify(osg::INFO)<<"    extentsLines="<<extentsLines<<std::endl;
        osg::notify(osg::INFO)<<"    targetResolution="<<targetResolution<<std::endl;
        
        nPixels = (int)ceil(extentsPixels/sqrt(osg::square(adfDstGeoTransform[1])+osg::square(adfDstGeoTransform[2])))+1;
        nLines = (int)ceil(extentsLines/sqrt(osg::square(adfDstGeoTransform[4])+osg::square(adfDstGeoTransform[5])))+1;

        osg::notify(osg::INFO)<<"        target computed resolution "<<targetResolution<<" nPixels="<<nPixels<<" nLines="<<nLines<<std::endl;
        
    }

    
    GDALDestroyGenImgProjTransformer( hTransformArg );

    GDALDataType eDT = GDALGetRasterDataType(GDALGetRasterBand(_sourceData->_gdalDataset,1));
    

/* --------------------------------------------------------------------- */
/*    Create the file                                                    */
/* --------------------------------------------------------------------- */

    int numSourceBands = GDALGetRasterCount(_sourceData->_gdalDataset);
    int numDestinationBands = (numSourceBands >= 3) ? 4 : numSourceBands; // expand RGB to RGBA, but leave other formats unchanged

    GDALDatasetH hDstDS = GDALCreate( hDriver, filename.c_str(), nPixels, nLines, 
                         numDestinationBands , eDT,
                         0 );
    
    if( hDstDS == NULL )
        return NULL;
        
        

/* -------------------------------------------------------------------- */
/*      Write out the projection definition.                            */
/* -------------------------------------------------------------------- */
    GDALSetProjection( hDstDS, cs->getCoordinateSystem().c_str() );
    GDALSetGeoTransform( hDstDS, adfDstGeoTransform );


// Set up the transformer along with the new datasets.

    hTransformArg = 
         GDALCreateGenImgProjTransformer( _sourceData->_gdalDataset,_sourceData->_cs->getCoordinateSystem().c_str(),
                                          hDstDS, cs->getCoordinateSystem().c_str(),
                                          TRUE, 0.0, 1 );

    GDALTransformerFunc pfnTransformer = GDALGenImgProjTransform;

    
    osg::notify(osg::INFO)<<"Setting projection "<<cs->getCoordinateSystem()<<std::endl;

/* -------------------------------------------------------------------- */
/*      Copy the color table, if required.                              */
/* -------------------------------------------------------------------- */
    GDALColorTableH hCT;

    hCT = GDALGetRasterColorTable( GDALGetRasterBand(_sourceData->_gdalDataset,1) );
    if( hCT != NULL )
        GDALSetRasterColorTable( GDALGetRasterBand(hDstDS,1), hCT );

/* -------------------------------------------------------------------- */
/*      Setup warp options.                                             */
/* -------------------------------------------------------------------- */
    GDALWarpOptions *psWO = GDALCreateWarpOptions();

    psWO->hSrcDS = _sourceData->_gdalDataset;
    psWO->hDstDS = hDstDS;

    psWO->pfnTransformer = pfnTransformer;
    psWO->pTransformerArg = hTransformArg;

    if(osg::isNotifyEnabled(osg::NotifySeverity(osg::INFO)))
      psWO->pfnProgress = GDALTermProgress;
    else 
      psWO->pfnProgress = GDALDummyProgress;
      
/* -------------------------------------------------------------------- */
/*      Setup band mapping.                                             */
/* -------------------------------------------------------------------- */
    psWO->nBandCount = numSourceBands;//numDestinationBands;
    psWO->panSrcBands = (int *) CPLMalloc(numDestinationBands*sizeof(int));
    psWO->panDstBands = (int *) CPLMalloc(numDestinationBands*sizeof(int));

    int i;
    for(i = 0; i < psWO->nBandCount; i++ )
    {
        psWO->panSrcBands[i] = i+1;
        psWO->panDstBands[i] = i+1;
    }


/* -------------------------------------------------------------------- */
/*      Setup no datavalue                                              */
/* -----------------------------------------------------`--------------- */

    psWO->padfSrcNoDataReal = (double*) CPLMalloc(psWO->nBandCount*sizeof(double));
    psWO->padfSrcNoDataImag = (double*) CPLMalloc(psWO->nBandCount*sizeof(double));

    psWO->padfDstNoDataReal = (double*) CPLMalloc(psWO->nBandCount*sizeof(double));
    psWO->padfDstNoDataImag = (double*) CPLMalloc(psWO->nBandCount*sizeof(double));

    for(i = 0; i < psWO->nBandCount; i++ )
    {
        int success = 0;
        GDALRasterBand* band = (i<numSourceBands) ? _sourceData->_gdalDataset->GetRasterBand(i+1) : 0;
        double noDataValue = band ? band->GetNoDataValue(&success) : 0.0;
        double new_noDataValue = 0;
        if (success)
        {
            osg::notify(osg::INFO)<<"\tassinging no data value "<<noDataValue<<" to band "<<i+1<<std::endl;

            psWO->padfSrcNoDataReal[i] = noDataValue;
            psWO->padfSrcNoDataImag[i] = 0.0;
            psWO->padfDstNoDataReal[i] = new_noDataValue;
            psWO->padfDstNoDataImag[i] = 0.0;

            GDALRasterBandH dest_band = GDALGetRasterBand(hDstDS,i+1);
            GDALSetRasterNoDataValue( dest_band, new_noDataValue);
        }
        else
        {
            psWO->padfSrcNoDataReal[i] = 0.0;
            psWO->padfSrcNoDataImag[i] = 0.0;
            psWO->padfDstNoDataReal[i] = new_noDataValue;
            psWO->padfDstNoDataImag[i] = 0.0;

            GDALRasterBandH dest_band = GDALGetRasterBand(hDstDS,i+1);
            GDALSetRasterNoDataValue( dest_band, new_noDataValue);
        }
    }    

    psWO->papszWarpOptions = (char**)CPLMalloc(2*sizeof(char*));
    psWO->papszWarpOptions[0] = strdup("INIT_DEST=NO_DATA");
    psWO->papszWarpOptions[1] = 0;
    
    if (numDestinationBands==4)
    {
/*    
        GDALSetRasterColorInterpretation( 
            GDALGetRasterBand( hDstDS, numDestinationBands ), 
            GCI_AlphaBand );
*/            
        psWO->nDstAlphaBand = numDestinationBands;
    }

/* -------------------------------------------------------------------- */
/*      Initialize and execute the warp.                                */
/* -------------------------------------------------------------------- */
    GDALWarpOperation oWO;

    if( oWO.Initialize( psWO ) == CE_None )
    {
        oWO.ChunkAndWarpImage( 0, 0, 
                               GDALGetRasterXSize( hDstDS ),
                               GDALGetRasterYSize( hDstDS ) );
    }

    osg::notify(osg::INFO)<<"new projection is "<<GDALGetProjectionRef(hDstDS)<<std::endl;

/* -------------------------------------------------------------------- */
/*      Cleanup.                                                        */
/* -------------------------------------------------------------------- */
    GDALDestroyGenImgProjTransformer( hTransformArg );
    
#if 0
    int anOverviewList[4] = { 2, 4, 8, 16 };
    GDALBuildOverviews( hDstDS, "AVERAGE", 4, anOverviewList, 0, NULL, 
                            GDALTermProgress/*GDALDummyProgress*/, NULL );
#endif

    GDALClose( hDstDS );
    
    Source* newSource = new Source;
    newSource->_type = _type;
    newSource->_filename = filename;
    newSource->_temporaryFile = true;
    newSource->_cs = cs;

    newSource->_coordinateSystemPolicy = _coordinateSystemPolicy;
    newSource->_geoTransformPolicy = _geoTransformPolicy;

    newSource->_minLevel = _minLevel;
    newSource->_maxLevel = _maxLevel;
    newSource->_layer = _layer;

    newSource->_requiredResolutions = _requiredResolutions;

    newSource->_numValuesX = nPixels;
    newSource->_numValuesY = nLines;
    newSource->_geoTransform.set( adfDstGeoTransform[1],    adfDstGeoTransform[4],      0.0,    0.0,
                                  adfDstGeoTransform[2],    adfDstGeoTransform[5],      0.0,    0.0,
                                  0.0,                      0.0,                        1.0,    0.0,
                                  adfDstGeoTransform[0],    adfDstGeoTransform[3],      0.0,    1.0);

    newSource->computeExtents();

    // reload the newly created file.
    newSource->loadSourceData();
                              
    return newSource;
}

void Source::consolodateRequiredResolutions()
{
    if (_requiredResolutions.size()<=1) return;

    ResolutionList consolodated;
    
    ResolutionList::iterator itr = _requiredResolutions.begin();
    
    double minResX = itr->_resX;
    double minResY = itr->_resY;
    double maxResX = itr->_resX;
    double maxResY = itr->_resY;
    ++itr;
    for(;itr!=_requiredResolutions.end();++itr)
    {
        minResX = osg::minimum(minResX,itr->_resX);
        minResY = osg::minimum(minResY,itr->_resY);
        maxResX = osg::maximum(maxResX,itr->_resX);
        maxResY = osg::maximum(maxResY,itr->_resY);
    }
    
    double currResX = minResX;
    double currResY = minResY;
    while (currResX<=maxResX && currResY<=maxResY)
    {
        consolodated.push_back(ResolutionPair(currResX,currResY));
        currResX *= 2.0f;
        currResY *= 2.0f;
    }
    

    _requiredResolutions.swap(consolodated);
}

void Source::buildOverviews()
{
    return;

    if (_sourceData.valid() && _sourceData->_gdalDataset )
    {

        int anOverviewList[4] = { 2, 4, 8, 16 };
        GDALBuildOverviews( _sourceData->_gdalDataset, "AVERAGE", 4, anOverviewList, 0, NULL, 
                                GDALTermProgress/*GDALDummyProgress*/, NULL );

    }
}


template<class T>
struct DerefLessFunctor
{
    bool operator () (const T& lhs, const T& rhs)
    {
        if (!lhs || !rhs) return lhs<rhs;
        if (lhs->getLayer() < rhs->getLayer()) return true;
        if (rhs->getLayer() < lhs->getLayer()) return false;
        return (lhs->getSortValue() > rhs->getSortValue());
    }
};

void CompositeSource::setSortValueFromSourceDataResolution()
{
    for(SourceList::iterator sitr=_sourceList.begin();sitr!=_sourceList.end();++sitr)
    {
        (*sitr)->setSortValueFromSourceDataResolution();
    }
        

    for(ChildList::iterator citr=_children.begin();citr!=_children.end();++citr)
    {
        (*citr)->setSortValueFromSourceDataResolution();
    }
}

void CompositeSource::sort()
{
    // sort the sources.
    std::sort(_sourceList.begin(),_sourceList.end(),DerefLessFunctor< osg::ref_ptr<Source> >());
    
    // sort the composite sources internal data
    for(ChildList::iterator itr=_children.begin();itr!=_children.end();++itr)
    {
        if (itr->valid()) (*itr)->sort();
    }
}
