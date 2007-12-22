#include "ExtrudeVisitor.h"                  

#include <osg/Geode>
#include <osg/Notify>
#include <osg/PrimitiveSet>

#include "EdgeCollector.h"
#include "ReversePrimitiveFunctor.h"

#include "ConvertVec.h"
#include "ArrayFunctor.h"





struct ExtrudeRangeOperator
{
    template <typename ArrayType>
	void process(ArrayType & array) 
	{   
    	typedef typename ArrayType::ElementDataType ElementDataType;
    	    	
		ElementDataType convertedExtrudeVector;
		ConvertVec<osg::Vec3, ElementDataType>::convert(_extrudeVector, convertedExtrudeVector);
		
    	typename ArrayType::iterator it = array.begin();
    	std::advance(it, _begin);
    	
    	typename ArrayType::iterator end = it;
    	std::advance(end, _count);
    	
	    for (; it < end; ++it)
	    	(*it) += convertedExtrudeVector;
	}

    unsigned int _begin;
    unsigned int _count;
    
    osg::Vec3 _extrudeVector;
};
typedef ArrayFunctor<ExtrudeRangeOperator> ExtrudeRangeFunctor;

struct DuplicateOperator
{
	template <typename ArrayType>
	void process(ArrayType & array) 
	{   
		std::size_t size = array.size();
		array.resize(size * 2);
		
		typename ArrayType::iterator it = array.begin(); 
		std::advance(it, size);
		std::copy(array.begin(), it, it);
	}
};   
typedef ArrayFunctor<DuplicateOperator> DuplicateFunctor;

struct DuplicateIndexOperator
{ 
    template <typename ArrayType>
	void process(ArrayType & array) 
	{   
    	std::size_t size = array.size();
    	array.resize(size + _indexArray->size());
    	
    	osg::UIntArray::iterator it, end = _indexArray->end();
		for (it = _indexArray->begin(); it < end; ++it, ++size)
	    	array[size] = array[*it];
	}
        
    osg::ref_ptr<osg::UIntArray> _indexArray;
};
typedef ArrayFunctor<DuplicateIndexOperator> DuplicateIndexFunctor;




struct OffsetIndices
{
	OffsetIndices(unsigned int & offset) : _offset(offset) {}
	void operator() (osg::ref_ptr<osg::PrimitiveSet> & primitive) { primitive->offsetIndices(_offset); }
  
	unsigned int _offset;
};




// ** search BoundaryEdgeloop in the geometry, extrude this loop
// **  and create primitiveSet to link original loop and extruded loop
void ExtrudeVisitor::extrude(osg::Geometry& geometry, osg::Vec3 & extrudeVector)
{
//    osg::notify(osg::INFO)<<"****************Extruder : Start ************"<<std::endl;

    unsigned int originalVertexArraySize = geometry.getVertexArray()->getNumElements();
    
    
    
    // ** collect Edge
//    osg::notify(osg::INFO)<<"****************Extruder : collect Points, Edges, Triangles"<<std::endl;
    EdgeCollector ec;
    ec.setGeometry(&geometry);
    if (ec._triangleSet.size() == 0) return;
//    osg::notify(osg::INFO)<<"****************Extruder : found " << ec._pointSet.size() << " Points"<<std::endl;
//    osg::notify(osg::INFO)<<"****************Extruder : found " << ec._edgeSet.size() << " Edges"<<std::endl;
//    osg::notify(osg::INFO)<<"****************Extruder : found " << ec._triangleSet.size() << " Triangles"<<std::endl;
    
    
    
    
    // ** collect Boundary Edge
//    osg::notify(osg::INFO)<<"****************Extruder : collect Boundary Edges"<<std::endl;
    EdgeCollector::EdgeList edgeList;
    ec.getBoundaryEdgeList(edgeList);
//    osg::notify(osg::INFO)<<"****************Extruder : found " << edgeList.size() << " Boundary Edges"<<std::endl;
    
    
    
    // ** collect Edgeloop
//    osg::notify(osg::INFO)<<"****************Extruder : collect Boundary Edgeloops"<<std::endl;
    EdgeCollector::EdgeloopList edgeloopList;
    if (ec.extractBoundaryEdgeloopList(edgeList, edgeloopList) == false)
    {
        osg::notify(osg::WARN) << "Extruder: fail to collect Edgeloop. Extrusion Canceled.\n\n\n" << std::endl;
        return;
    }
//    osg::notify(osg::INFO)<<"****************Extruder : found " << edgeloopList.size() << " Boundary Edgesloop"<<std::endl;
    
    
    // ** get IndexArray of each Edgeloop
    typedef std::list<osg::ref_ptr<osg::UIntArray> > IndexArrayList;
    IndexArrayList originalIndexArrayList;
    IndexArrayList extrudedIndexArrayList;
        
    EdgeCollector::EdgeloopList::iterator elIt, elEnd = edgeloopList.end();
    for (elIt = edgeloopList.begin(); elIt != elEnd; ++elIt)
    {
    	originalIndexArrayList.push_back((*elIt)->toIndexArray());
    	extrudedIndexArrayList.push_back(new osg::UIntArray(*originalIndexArrayList.back().get()));
    }
    
    _mode = Replace;
    
    // ** replace geometry by extruded geometry
    if (_mode == Replace)
    {
    	// ** extrude vertexArray
//	    osg::notify(osg::INFO)<<"****************Extruder : Duplicate Edgeloop and Extrude VertexArray ************"<<std::endl;
	    
	    
	    unsigned int currentVertexArraySize = originalVertexArraySize;
	    
	    DuplicateIndexFunctor dif;
	    IndexArrayList::iterator iaIt, iaEnd = originalIndexArrayList.end();
		for (iaIt = originalIndexArrayList.begin(); iaIt != iaEnd; ++iaIt)
		{
			osg::UIntArray & indexArray = *(iaIt->get());
			
			// ** duplicate vertex indexed by indexArray
		    dif._indexArray = &indexArray;
		    geometry.getVertexArray()->accept(dif);
		    		    
		    // ** remap indexArray
		    unsigned int size = indexArray.size();
		    for (unsigned int i=0; i<size; ++i)
		    	indexArray[i] = currentVertexArraySize + i;
		    
		    currentVertexArraySize += size;
		}
	    
		// ** extrude original vertex and keep original value in duplicate vertex
		ExtrudeRangeFunctor erf;
	    erf._extrudeVector = extrudeVector;
	    erf._begin = 0;
	    erf._count = originalVertexArraySize;
	    
	    geometry.getVertexArray()->accept(erf);
    }
    else //if (_mode == Merge)
    {
	    // ** duplicate vertexArray
//	    osg::notify(osg::INFO)<<"****************Extruder : Duplicate and extrude VertexArray ************"<<std::endl;
	    
	    // ** duplicate all vertex
	    DuplicateFunctor df;
	    geometry.getVertexArray()->accept(df);
	    
	    // ** duplicate edgeloop index
	    IndexArrayList::iterator iaIt, iaEnd = originalIndexArrayList.end();
		for (iaIt = originalIndexArrayList.begin(); iaIt != iaEnd; ++iaIt)
		{
			osg::UIntArray & indexArray = *(iaIt->get());
			
		    // ** remap indexArray
		    unsigned int size = indexArray.size();
		    for (unsigned int i=0; i<size; ++i)
		    	indexArray[i] += originalVertexArraySize;
		}
	    
	    // ** extrude original vertex and keep original value in duplicate vertex
	    ExtrudeRangeFunctor erf;
	    erf._extrudeVector = extrudeVector;
	    erf._begin = 0;
	    erf._count = originalVertexArraySize;
	    geometry.getVertexArray()->accept(erf);

	    // ** offset primitive's indices to draw duplicated vertex
	    osg::Geometry::PrimitiveSetList & psl = geometry.getPrimitiveSetList();
	    std::for_each(psl.begin(), psl.end(), OffsetIndices(originalVertexArraySize));
	
    }
    
    
    
    
    
    
    bool reverseTopShape = true;//needReverseTopShape(extrudeVector, ); TODO
    
//    osg::notify(osg::INFO)<<"****************Extruder : Make top primitiveSet List ************"<<std::endl;
    {
        osg::Geometry::PrimitiveSetList newPsl;
        
        // ** for each primitive in geometry, we duplicate this primitive and reverse it if need
        osg::Geometry::PrimitiveSetList & psl = geometry.getPrimitiveSetList();
        osg::Geometry::PrimitiveSetList::iterator it, end = psl.end();
        for (it = psl.begin(); it != end; ++it)
        {
            osg::ref_ptr<osg::PrimitiveSet> newPs;
            
            // ** duplicate
            if (reverseTopShape)
            {
                ReversePrimitiveFunctor rpf;
                (*it)->accept(rpf);
                
                newPs = rpf.getReversedPrimitiveSet();
            }
            else
            {
                newPs = static_cast<osg::PrimitiveSet*>((*it)->clone(osg::CopyOp::SHALLOW_COPY));
            }
            
            if (newPs.valid())
            {
	            newPsl.push_back(newPs);
            }
        }
        
        // ** Replace by or Merge new Primitives
        if (_mode == Replace)
        {
        	psl = newPsl;
        }
        else //if (_mode == Merge)
        {
        	psl.insert(psl.end(), newPsl.begin(), newPsl.end());
        }
    }
    
    
    
    
    
    
//    osg::notify(osg::INFO)<<"****************Extruder : link original and extruded Boundary Edgeloops ************"<<std::endl;
    // ** link original and extruded Boundary Edgeloops 
    
    IndexArrayList::iterator oit, oend = originalIndexArrayList.end();
    IndexArrayList::iterator eit = extrudedIndexArrayList.begin();
    for (oit = originalIndexArrayList.begin(); oit != oend; ++oit, ++eit)
    {
    	
    	osg::ref_ptr<osg::DrawElementsUInt> de = linkAsTriangleStrip(*(*oit), *(*eit));
    	
    	// ** if bottom shape have up normal, we need to reverse the extruded face   
    	if (reverseTopShape == false)
    	{
    		ReversePrimitiveFunctor rpf;
            de->accept(rpf);
            
            geometry.addPrimitiveSet(rpf._reversedPrimitiveSet.get());
    	}
    	else
    	{
    		// ** add the extruded face
    		geometry.addPrimitiveSet(de.get());
    	}
    }
    
//    osg::notify(osg::INFO)<<"****************Extruder : finish ************"<<std::endl;
}

osg::DrawElementsUInt * ExtrudeVisitor::linkAsTriangleStrip(osg::UIntArray & originalIndexArray,
                                                            osg::UIntArray & extrudeIndexArray)
{
    // ** create new PrimitiveSet
    osg::DrawElementsUInt * de(new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLE_STRIP));
    de->reserve(originalIndexArray.size() * 2 + 2);
    
    // ** and link original edgeloop with extruded edgeloop
    osg::UIntArray::iterator oIt, oEnd = originalIndexArray.end();
    osg::UIntArray::iterator eIt = extrudeIndexArray.begin();
    for (oIt = originalIndexArray.begin(); oIt != oEnd; ++oIt, ++eIt)
    {
        de->push_back(*oIt);
        de->push_back(*eIt);
    }
    de->push_back(*(originalIndexArray.begin()));
    de->push_back(*(extrudeIndexArray.begin()));
    
    return de;
}

void ExtrudeVisitor::apply(osg::Geode & node)
{
    unsigned int numDrawable = node.getNumDrawables();
    
    osg::Vec3 extrudeVector(0.0f, 0.0f, 0.0f);
    
    for (unsigned int i = 0; i < numDrawable; ++i)
    {
        extrudeVector.z() = (i % 5) + 10;
        osg::Geometry * geo = dynamic_cast<osg::Geometry *>(node.getDrawable(i));
        
        if (geo)
        {
            extrude(*geo, extrudeVector);
        }
    }
    
    osg::NodeVisitor::apply(node);
}

//bool ExtrudeVisitor:needReverseTopShape(osg::Vec3 & extrudeVector, osg::Vec3 p1, osg::Vec3 p2, osg::Vec3 p3)
//{
//	osg::Vec3 normal = (p2-p1) ^ (p3-p1);
//	
//	return ((extrudeVector * normal) >= 0);
//}