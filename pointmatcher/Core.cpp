// vim: ts=4:sw=4:noexpandtab
/*

Copyright (c) 2010--2011,
François Pomerleau and Stephane Magnenat, ASL, ETHZ, Switzerland
You can contact the authors at <f dot pomerleau at gmail dot com> and
<stephane at magnenat dot net>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ETH-ASL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "PointMatcher.h"
#include "PointMatcherPrivate.h"
#include "Timer.h"

#include "LoggerImpl.h"
#include "TransformationsImpl.h"
#include "DataPointsFiltersImpl.h"
#include "MatchersImpl.h"
#include "OutlierFiltersImpl.h"
#include "ErrorMinimizersImpl.h"
#include "TransformationCheckersImpl.h"
#include "InspectorsImpl.h"

#include <cassert>
#include <iostream>
#include <limits>

using namespace std;
using namespace PointMatcherSupport;

namespace PointMatcherSupport
{
	boost::mutex loggerMutex;
	std::shared_ptr<Logger> logger;
	
	//! Set a new logger, protected by a mutex
	void setLogger(Logger* newLogger)
	{
		boost::mutex::scoped_lock lock(loggerMutex);
		logger.reset(newLogger);
	}
}

// DataPoints

//! Return whether there is a label named text
template<typename T>
bool PointMatcher<T>::DataPoints::Labels::contains(const std::string& text) const
{
	for (const_iterator it(this->begin()); it != this->end(); ++it)
	{
		if (it->text == text)
			return true;
	}
	return false;
}

//! Return the sum of the spans of each label
template<typename T>
size_t PointMatcher<T>::DataPoints::Labels::totalDim() const
{
	size_t dim(0);
	for (const_iterator it(this->begin()); it != this->end(); ++it)
		dim += it->span;
	return dim;
}

//! Construct a label from a given name and number of data dimensions it spans
template<typename T>
PointMatcher<T>::DataPoints::Label::Label(const std::string& text, const size_t span):
	text(text),
	span(span)
{}

//! Construct an empty point cloud
template<typename T>
PointMatcher<T>::DataPoints::DataPoints()
{}

//! Construct a point cloud from existing descriptions
template<typename T>
PointMatcher<T>::DataPoints::DataPoints(const Labels& featureLabels, const Labels& descriptorLabels, const size_t pointCount):
	featureLabels(featureLabels),
	descriptorLabels(descriptorLabels)
{
	features.resize(featureLabels.totalDim(), pointCount);
	descriptors.resize(descriptorLabels.totalDim(), pointCount);
}

//! Construct a point cloud from existing features without any descriptor
template<typename T>
PointMatcher<T>::DataPoints::DataPoints(const Matrix& features, const Labels& featureLabels):
	features(features),
	featureLabels(featureLabels)
{}

//! Construct a point cloud from existing features and descriptors
template<typename T>
PointMatcher<T>::DataPoints::DataPoints(const Matrix& features, const Labels& featureLabels, const Matrix& descriptors, const Labels& descriptorLabels):
	features(features),
	featureLabels(featureLabels),
	descriptors(descriptors),
	descriptorLabels(descriptorLabels)
{}

//! Add an other point cloud after the current one
template<typename T>
void PointMatcher<T>::DataPoints::concatenate(const DataPoints dp)
{
	const int nbPoints1 = this->features.cols();
	const int nbPoints2 = dp.features.cols();
	const int nbPointsTotal = nbPoints1 + nbPoints2;

	const int dimFeat = this->features.rows();
	if(dimFeat != dp.features.rows())
	{
		stringstream errorMsg;
		errorMsg << "Cannot concatenate DataPoints because the dimension of the features are not the same. Actual dimension: " << dimFeat << " New dimension: " << dp.features.rows(); 
		throw InvalidField(errorMsg.str());
	}
	
	Matrix combinedFeat(dimFeat, nbPointsTotal);
	combinedFeat.leftCols(nbPoints1) = this->features;
	combinedFeat.rightCols(nbPoints2) = dp.features;
	DataPoints dpOut(combinedFeat, this->featureLabels);
	
	for(unsigned i = 0; i < this->descriptorLabels.size(); i++)
	{
		const string name = this->descriptorLabels[i].text;
		const int dimDesc = this->descriptorLabels[i].span;
		if(dp.descriptorExists(name, dimDesc) == true)
		{
			Matrix mergedDesc(dimDesc, nbPointsTotal);
			mergedDesc.leftCols(nbPoints1) = this->getDescriptorViewByName(name);
			mergedDesc.rightCols(nbPoints2) = dp.getDescriptorViewByName(name);
			dpOut.addDescriptor(name, mergedDesc);
		}
	}

	this->features.swap(dpOut.features);
	this->featureLabels = dpOut.featureLabels;
	this->descriptors.swap(dpOut.descriptors);
	this->descriptorLabels = dpOut.descriptorLabels;
}



//! Makes sure a feature of a given name exists, if present, check its dimensions
template<typename T>
void PointMatcher<T>::DataPoints::allocateFeature(const std::string& name, const unsigned dim)
{
	allocateField(name, dim, featureLabels, features);
}

//! Add a feature by name, remove first if already exists
template<typename T>
void PointMatcher<T>::DataPoints::addFeature(const std::string& name, const Matrix& newFeature)
{
	addField(name, newFeature, featureLabels, features);
}

//! Get feature by name, return a matrix containing a copy of the requested feature
template<typename T>
typename PointMatcher<T>::Matrix PointMatcher<T>::DataPoints::getFeatureCopyByName(const std::string& name) const
{
	return Matrix(getFeatureViewByName(name));
}

//! Get a const view on a feature by name, throw an exception if it does not exist
template<typename T>
typename PointMatcher<T>::DataPoints::ConstView PointMatcher<T>::DataPoints::getFeatureViewByName(const std::string& name) const
{
	return getConstViewByName(name, featureLabels, features);
}

//! Look if a feature with a given name exist
template<typename T>
bool PointMatcher<T>::DataPoints::featureExists(const std::string& name) const
{
	return fieldExists(name, 0, featureLabels);
}

//! Look if a feature with a given name and dimension exist
template<typename T>
bool PointMatcher<T>::DataPoints::featureExists(const std::string& name, const unsigned dim) const
{
	return fieldExists(name, dim, featureLabels);
}

//! Return the dimension of a feature with a given name. Return 0 if the name is not found
template<typename T>
unsigned PointMatcher<T>::DataPoints::getFeatureDimension(const std::string& name) const
{
	return getFieldDimension(name, featureLabels);
}

//! Return the starting row of a feature with a given name. Return 0 if the name is not found
template<typename T>
unsigned PointMatcher<T>::DataPoints::getFeatureStartingRow(const std::string& name) const
{
	return getFieldStartingRow(name, featureLabels);
}


//! Get a view on a feature by name, throw an exception if it does not exist
template<typename T>
typename PointMatcher<T>::DataPoints::View PointMatcher<T>::DataPoints::getFeatureViewByName(const std::string& name)
{
	return getViewByName(name, featureLabels, features);
}

//! Makes sure a descriptor of a given name exists, if present, check its dimensions
template<typename T>
void PointMatcher<T>::DataPoints::allocateDescriptor(const std::string& name, const unsigned dim)
{
	allocateField(name, dim, descriptorLabels, descriptors);
}

//! Add a descriptor by name, remove first if already exists
template<typename T>
void PointMatcher<T>::DataPoints::addDescriptor(const std::string& name, const Matrix& newDescriptor)
{
	addField(name, newDescriptor, descriptorLabels, descriptors);
}

//! Get descriptor by name, return a matrix containing a copy of the requested descriptor
template<typename T>
typename PointMatcher<T>::Matrix PointMatcher<T>::DataPoints::getDescriptorCopyByName(const std::string& name) const
{
	return Matrix(getDescriptorViewByName(name));
}

//! Get a const view on a descriptor by name, throw an exception if it does not exist
template<typename T>
typename PointMatcher<T>::DataPoints::ConstView PointMatcher<T>::DataPoints::getDescriptorViewByName(const std::string& name) const
{
	return getConstViewByName(name, descriptorLabels, descriptors);
}


//! Get a view on a descriptor by name, throw an exception if it does not exist
template<typename T>
typename PointMatcher<T>::DataPoints::View PointMatcher<T>::DataPoints::getDescriptorViewByName(const std::string& name)
{
	return getViewByName(name, descriptorLabels, descriptors);
}

//! Look if a descriptor with a given name exist
template<typename T>
bool PointMatcher<T>::DataPoints::descriptorExists(const std::string& name) const
{
	return fieldExists(name, 0, descriptorLabels);
}

//! Look if a descriptor with a given name and dimension exist
template<typename T>
bool PointMatcher<T>::DataPoints::descriptorExists(const std::string& name, const unsigned dim) const
{
	return fieldExists(name, dim, descriptorLabels);
}

//! Return the dimension of a descriptor with a given name. Return 0 if the name is not found
template<typename T>
unsigned PointMatcher<T>::DataPoints::getDescriptorDimension(const std::string& name) const
{
	return getFieldDimension(name, descriptorLabels);
}

//! Return the starting row of a descriptor with a given name. Return 0 if the name is not found
template<typename T>
unsigned PointMatcher<T>::DataPoints::getDescriptorStartingRow(const std::string& name) const
{
	return getFieldStartingRow(name, descriptorLabels);
}


//! Makes sure a field of a given name exists, if present, check its dimensions
template<typename T>
void PointMatcher<T>::DataPoints::allocateField(const std::string& name, const unsigned dim, Labels& labels, Matrix& data)
{
	if (fieldExists(name, 0, labels))
	{
		const unsigned descDim(getFieldDimension(name, labels));
		if (descDim != dim)
		{
			throw InvalidField(
				(boost::format("The existing field %1% has dimension %2%, different than requested dimension %3%") % name % descDim % dim).str()
			);
		}
	}
	else
	{
		const int oldDim(data.rows());
		const int totalDim(oldDim + dim);
		const int pointCount(features.cols());
		Matrix tmpData(data);
		data.resize(totalDim, pointCount);
		if (oldDim != 0)
			data.topRows(oldDim) = tmpData;
		labels.push_back(Label(name, dim));
	}
}

//! Add a descriptor or feature by name, remove first if already exists
template<typename T>
void PointMatcher<T>::DataPoints::addField(const std::string& name, const Matrix& newField, Labels& labels, Matrix& data)
{
	const int newFieldDim = newField.rows();
	const int newPointCount = newField.cols();
	const int pointCount = features.cols();

	if (newField.rows() == 0)
		return;

	// Replace if the field exists
	if (fieldExists(name, 0, labels))
	{
		const int fieldDim = getFieldDimension(name, labels);
		
		if(fieldDim == newFieldDim)
		{
			// Ensure that the number of points in the point cloud and in the field are the same
			if(pointCount == newPointCount)
			{
				const int row = getFieldStartingRow(name, labels);
				data.block(row, 0, fieldDim, pointCount) = newField;
			}
			else
			{
				stringstream errorMsg;
				errorMsg << "The field " << name << " cannot be added because the number of points is not the same. Old point count: " << pointCount << "new: " << newPointCount;
				throw InvalidField(errorMsg.str());
			}
		}
		else
		{
			stringstream errorMsg;
			errorMsg << "The field " << name << " already exists but could not be added because the dimension is not the same. Old dim: " << fieldDim << " new: " << newFieldDim;
			throw InvalidField(errorMsg.str());
		}
	}
	else // Add at the end if it is a new field
	{
		if(pointCount == newPointCount)
		{
			const int oldFieldDim(data.rows());
			const int totalDim = oldFieldDim + newFieldDim;
			Matrix tmpFields(data);
			data.resize(totalDim, pointCount);
			if (oldFieldDim > 0)
				data.topRows(oldFieldDim) = tmpFields;
			data.bottomRows(newFieldDim) = newField;
			labels.push_back(Label(name, newFieldDim));
		}
		else
		{
			stringstream errorMsg;
			errorMsg << "The field " << name << " cannot be added because the number of points is not the same. Old point count: " << pointCount << " new: " << newPointCount;
			throw InvalidField(errorMsg.str());
		}
	}
}

//! Get a const view on a matrix by name, throw an exception if it does not exist
template<typename T>
typename PointMatcher<T>::DataPoints::ConstView PointMatcher<T>::DataPoints::getConstViewByName(const std::string& name, const Labels& labels, const Matrix& data) const
{
	unsigned row(0);
	for(auto it(labels.begin()); it != labels.end(); ++it)
	{
		if (it->text == name)
			return descriptors.block(row, 0, it->span, descriptors.cols());
		row += it->span;
	}
	throw InvalidField("Field " + name + " not found");
}

//! Get a view on a matrix by name, throw an exception if it does not exist
template<typename T>
typename PointMatcher<T>::DataPoints::View PointMatcher<T>::DataPoints::getViewByName(const std::string& name, const Labels& labels, Matrix& data)
{
	unsigned row(0);
	for(auto it(labels.begin()); it != labels.end(); ++it)
	{
		if (it->text == name)
			return descriptors.block(row, 0, it->span, descriptors.cols());
		row += it->span;
	}
	throw InvalidField("Field " + name + " not found");
}

//! Look if a descriptor or a feature with a given name and dimension exist
template<typename T>
bool PointMatcher<T>::DataPoints::fieldExists(const std::string& name, const unsigned dim, const Labels& labels) const
{
	for(auto it(labels.begin()); it != labels.end(); ++it)
	{
		if (it->text == name)
		{
			if (dim == 0 || it->span == dim)
				return true;
			else
				return false;
		}
	}
	return false;
}


//! Return the dimension of a feature or a descriptor with a given name. Return 0 if the name is not found
template<typename T>
unsigned PointMatcher<T>::DataPoints::getFieldDimension(const std::string& name, const Labels& labels) const
{
	for(auto it(labels.begin()); it != labels.end(); ++it)
	{
		if (it->text == name)
			return it->span;
	}
	return 0;
}


//! Return the starting row of a feature or a descriptor with a given name. Return 0 if the name is not found
template<typename T>
unsigned PointMatcher<T>::DataPoints::getFieldStartingRow(const std::string& name, const Labels& labels) const
{
	unsigned row(0);
	for(auto it(labels.begin()); it != labels.end(); ++it)
	{
		if (it->text == name)
			return row;
		row += it->span;
	}
	return 0;
}

template<typename T>
void swapDataPoints(typename PointMatcher<T>::DataPoints& a, typename PointMatcher<T>::DataPoints& b)
{
	a.features.swap(b.features);
	swap(a.featureLabels, b.featureLabels);
	a.descriptors.swap(b.descriptors);
	swap(a.descriptorLabels, b.descriptorLabels);
}

// Matches

//! Construct empty matches
template<typename T>
PointMatcher<T>::Matches::Matches() {}

//! Construct matches from distances to and identifiers of closest points
template<typename T>
PointMatcher<T>::Matches::Matches(const Dists& dists, const Ids ids):
	dists(dists),
	ids(ids)
{}

//! Construct uninitialized matches from number of closest points (knn) and number of points (pointsCount)
template<typename T>
PointMatcher<T>::Matches::Matches(const int knn, const int pointsCount):
	dists(Dists(knn, pointsCount)),
	ids(Ids(knn, pointsCount))
{}

//! Get the distance at the T-ratio closest point
template<typename T>
T PointMatcher<T>::Matches::getDistsQuantile(const T quantile) const
{
	// build array
	vector<T> values;
	values.reserve(dists.rows() * dists.cols());
	for (int x = 0; x < dists.cols(); ++x)
		for (int y = 0; y < dists.rows(); ++y)
			if ((dists(y, x) != numeric_limits<T>::infinity()) && (dists(y, x) > 0))
				values.push_back(dists(y, x));
	if (values.size() == 0)
		throw ConvergenceError("no outlier to filter");
	
	// get quantile
	nth_element(values.begin(), values.begin() + (values.size() * quantile), values.end());
	return values[values.size() * quantile];
}

// Transformation

//! Construct without parameter
template<typename T>
PointMatcher<T>::Transformation::Transformation()
{}

//! Construct with parameters
template<typename T>
PointMatcher<T>::Transformation::Transformation(const std::string& className, const ParametersDoc paramsDoc, const Parameters& params):
	Parametrizable(className,paramsDoc,params)
{}

//! virtual destructor
template<typename T>
PointMatcher<T>::Transformation::~Transformation()
{} 

//! Apply this chain to cloud, using parameters, mutates cloud
template<typename T>
void PointMatcher<T>::Transformations::apply(DataPoints& cloud, const TransformationParameters& parameters) const
{
	DataPoints transformedCloud;
	for (TransformationsConstIt it = this->begin(); it != this->end(); ++it)
	{
		transformedCloud = (*it)->compute(cloud, parameters);
		swapDataPoints<T>(cloud, transformedCloud);
	}
}

// DataPointsFilter

//! Construct without parameter
template<typename T>
PointMatcher<T>::DataPointsFilter::DataPointsFilter()
{} 

//! Construct with parameters
template<typename T>
PointMatcher<T>::DataPointsFilter::DataPointsFilter(const std::string& className, const ParametersDoc paramsDoc, const Parameters& params):
	Parametrizable(className,paramsDoc,params)
{}

//! virtual destructor
template<typename T>
PointMatcher<T>::DataPointsFilter::~DataPointsFilter()
{} 

//! Init this filter
template<typename T>
void PointMatcher<T>::DataPointsFilter::init()
{}

//! Construct an empty chain
template<typename T>
PointMatcher<T>::DataPointsFilters::DataPointsFilters()
{}

//! Construct a chain from a YAML file
template<typename T>
PointMatcher<T>::DataPointsFilters::DataPointsFilters(std::istream& in)
{
	#ifdef HAVE_YAML_CPP
	
	YAML::Parser parser(in);
	YAML::Node doc;
	parser.GetNextDocument(doc);
	
	PointMatcher<T> pm;
	
	for(YAML::Iterator moduleIt = doc.begin(); moduleIt != doc.end(); ++moduleIt)
	{
		const YAML::Node& module(*moduleIt);
		push_back(pm.REG(DataPointsFilter).createFromYAML(module));
	}
	
	#endif // HAVE_YAML_CPP
}

//! Init the chain
template<typename T>
void PointMatcher<T>::DataPointsFilters::init()
{
	for (DataPointsFiltersIt it = this->begin(); it != this->end(); ++it)
	{
		(*it)->init();
	}
}

//! Apply this chain to cloud, mutates cloud
template<typename T>
void PointMatcher<T>::DataPointsFilters::apply(DataPoints& cloud)
{
	DataPoints filteredCloud;

	for (DataPointsFiltersIt it = this->begin(); it != this->end(); ++it)
	{
		const int pointsCount(cloud.features.cols());
		if (pointsCount == 0)
			throw ConvergenceError("no point to filter");
		
		filteredCloud = (*it)->filter(cloud);
		swapDataPoints<T>(cloud, filteredCloud);
		LOG_INFO_STREAM("in: " << pointsCount << " pts -> out: " << cloud.features.cols());
	}
}

// Matcher

//! Construct without parameter
template<typename T>
PointMatcher<T>::Matcher::Matcher():
	visitCounter(0)
{}

//! Construct with parameters
template<typename T>
PointMatcher<T>::Matcher::Matcher(const std::string& className, const ParametersDoc paramsDoc, const Parameters& params):
	Parametrizable(className,paramsDoc,params),
	visitCounter(0)
{}

//! virtual destructor
template<typename T>
PointMatcher<T>::Matcher::~Matcher()
{}

//! Reset the visit counter
template<typename T>
void PointMatcher<T>::Matcher::resetVisitCount()
{
	visitCounter = 0;
}

//! Return the visit counter
template<typename T>
unsigned long PointMatcher<T>::Matcher::getVisitCount() const
{
	return visitCounter;
}

// OutlierFilter

//! Construct without parameter
template<typename T>
PointMatcher<T>::OutlierFilter::OutlierFilter()
{}

//! Construct with parameters
template<typename T>
PointMatcher<T>::OutlierFilter::OutlierFilter(const std::string& className, const ParametersDoc paramsDoc, const Parameters& params):
	Parametrizable(className,paramsDoc,params)
{}

//! virtual destructor
template<typename T>
PointMatcher<T>::OutlierFilter::~OutlierFilter()
{}

//! Apply outlier-detection chain
template<typename T>
typename PointMatcher<T>::OutlierWeights PointMatcher<T>::OutlierFilters::compute(
	const typename PointMatcher<T>::DataPoints& filteredReading,
	const typename PointMatcher<T>::DataPoints& filteredReference,
	const typename PointMatcher<T>::Matches& input)
{
	if (this->empty())
	{
		// we do not have any filter, therefore we must put 0 weights for infinite distances
		OutlierWeights w(input.dists.rows(), input.dists.cols());
		for (int x = 0; x < w.cols(); ++x)
		{
			for (int y = 0; y < w.rows(); ++y)
			{
				if (input.dists(y, x) == numeric_limits<T>::infinity())
					w(y, x) = 0;
				else
					w(y, x) = 1;
			}
		}
		return w;
	}
	else
	{
		// apply filters, they should take care of infinite distances
		OutlierWeights w = (*this->begin())->compute(filteredReading, filteredReference, input);
		if (this->size() > 1)
		{
			for (OutlierFiltersConstIt it = (this->begin() + 1); it != this->end(); ++it)
				w = w.array() * (*it)->compute(filteredReading, filteredReference, input).array();
		}

		return w;
	}
}

// ErrorMinimizer, see ErrorMinimizers.cpp

// TranformationCheckers

template<typename T>
void PointMatcher<T>::TransformationCheckers::init(const TransformationParameters& parameters, bool& iterate)
{
	for (TransformationCheckersIt it = this->begin(); it != this->end(); ++it)
		(*it)->init(parameters, iterate);
}

template<typename T>
void PointMatcher<T>::TransformationCheckers::check(const TransformationParameters& parameters, bool& iterate)
{
	for (TransformationCheckersIt it = this->begin(); it != this->end(); ++it)
		(*it)->check(parameters, iterate);
}

// Inspector

//! Construct without parameter
template<typename T>
PointMatcher<T>::Inspector::Inspector()
{}

//! Construct with parameters
template<typename T>
PointMatcher<T>::Inspector::Inspector(const std::string& className, const ParametersDoc paramsDoc, const Parameters& params):
	Parametrizable(className,paramsDoc,params)
{}

//! virtual destructor
template<typename T>
PointMatcher<T>::Inspector::~Inspector()
{}

//! Start a new ICP operation or sequence
template<typename T>
void PointMatcher<T>::Inspector::init()
{}

// performance statistics

//! Add a value for statistics name, create it if new
template<typename T>
void PointMatcher<T>::Inspector::addStat(const std::string& name, double data)
{}

//! Dump all statistics in CSV format
template<typename T>
void PointMatcher<T>::Inspector::dumpStats(std::ostream& stream)
{}

//! Dump header for all statistics
template<typename T>
void PointMatcher<T>::Inspector::dumpStatsHeader(std::ostream& stream)
{}

// data statistics 
template<typename T>
void PointMatcher<T>::Inspector::dumpFilteredReference(const DataPoints& filteredReference)
{}

template<typename T>
void PointMatcher<T>::Inspector::dumpIteration(const size_t iterationCount, const TransformationParameters& parameters, const DataPoints& filteredReference, const DataPoints& reading, const Matches& matches, const OutlierWeights& outlierWeights, const TransformationCheckers& transformationCheckers)
{}

template<typename T>
void PointMatcher<T>::Inspector::finish(const size_t iterationCount)
{}

// algorithms

//! Protected contstructor, to prevent the creation of this object
template<typename T>
PointMatcher<T>::ICPChainBase::ICPChainBase():
	prefilteredReadingPtsCount(0),
	prefilteredKeyframePtsCount(0)
{}

//! virtual desctructor
template<typename T>
PointMatcher<T>::ICPChainBase::~ICPChainBase()
{
}

//! Clean chain up, empty all filters and delete associated objects
template<typename T>
void PointMatcher<T>::ICPChainBase::cleanup()
{
	transformations.clear();
	readingDataPointsFilters.clear();
	readingStepDataPointsFilters.clear();
	keyframeDataPointsFilters.clear();
	matcher.reset();
	outlierFilters.clear();
	errorMinimizer.reset();
	transformationCheckers.clear();
	inspector.reset();
}

//! Construct an ICP algorithm that works in most of the cases
template<typename T>
void PointMatcher<T>::ICPChainBase::setDefault()
{
	this->cleanup();
	
	this->transformations.push_back(new typename TransformationsImpl<T>::RigidTransformation());
	this->readingDataPointsFilters.push_back(new typename DataPointsFiltersImpl<T>::RandomSamplingDataPointsFilter());
	this->keyframeDataPointsFilters.push_back(new typename DataPointsFiltersImpl<T>::SamplingSurfaceNormalDataPointsFilter());
	this->outlierFilters.push_back(new typename OutlierFiltersImpl<T>::TrimmedDistOutlierFilter());
	this->matcher.reset(new typename MatchersImpl<T>::KDTreeMatcher());
	this->errorMinimizer.reset(new typename ErrorMinimizersImpl<T>::PointToPlaneErrorMinimizer());
	this->transformationCheckers.push_back(new typename TransformationCheckersImpl<T>::CounterTransformationChecker());
	this->transformationCheckers.push_back(new typename TransformationCheckersImpl<T>::DifferentialTransformationChecker());
	this->inspector.reset(new typename InspectorsImpl<T>::NullInspector);
}

//! Construct an ICP algorithm from a YAML file
template<typename T>
void PointMatcher<T>::ICPChainBase::loadFromYaml(std::istream& in)
{
	#ifdef HAVE_YAML_CPP
	
	this->cleanup();
	
	YAML::Parser parser(in);
	YAML::Node doc;
	parser.GetNextDocument(doc);
	
	PointMatcher<T> pm;
	
	createModulesFromRegistrar("readingDataPointsFilters", doc, pm.REG(DataPointsFilter), readingDataPointsFilters);
	createModulesFromRegistrar("readingStepDataPointsFilters", doc, pm.REG(DataPointsFilter), readingStepDataPointsFilters);
	createModulesFromRegistrar("keyframeDataPointsFilters", doc, pm.REG(DataPointsFilter), keyframeDataPointsFilters);
	//createModulesFromRegistrar("transformations", doc, pm.REG(Transformation), transformations);
	this->transformations.push_back(new typename TransformationsImpl<T>::RigidTransformation());
	createModuleFromRegistrar("matcher", doc, pm.REG(Matcher), matcher);
	createModulesFromRegistrar("outlierFilters", doc, pm.REG(OutlierFilter), outlierFilters);
	createModuleFromRegistrar("errorMinimizer", doc, pm.REG(ErrorMinimizer), errorMinimizer);
	createModulesFromRegistrar("transformationCheckers", doc, pm.REG(TransformationChecker), transformationCheckers);
	createModuleFromRegistrar("inspector", doc, pm.REG(Inspector),inspector);
	{
		boost::mutex::scoped_lock lock(loggerMutex);
		createModuleFromRegistrar("logger", doc, pm.REG(Logger), logger);
	}
	
	loadAdditionalYAMLContent(doc);
	
	#else // HAVE_YAML_CPP
	throw runtime_error("Yaml support not compiled in. Install yaml-cpp, configure build and recompile.");
	#endif // HAVE_YAML_CPP
}

//! Return the remaining number of points in reading after prefiltering but before the iterative process
template<typename T>
unsigned PointMatcher<T>::ICPChainBase::getPrefilteredReadingPtsCount() const
{
	return prefilteredReadingPtsCount;
}

//! Return the remaining number of points in the keyframe after prefiltering but before the iterative process
template<typename T>
unsigned PointMatcher<T>::ICPChainBase::getPrefilteredKeyframePtsCount() const
{
	return prefilteredReadingPtsCount;
}

#ifdef HAVE_YAML_CPP

template<typename T>
template<typename R>
void PointMatcher<T>::ICPChainBase::createModulesFromRegistrar(const std::string& regName, const YAML::Node& doc, const R& registrar, PointMatcherSupport::SharedPtrVector<typename R::TargetType>& modules)
{
	const YAML::Node *reg = doc.FindValue(regName);
	if (reg)
	{
		cout << regName << endl;
		for(YAML::Iterator moduleIt = reg->begin(); moduleIt != reg->end(); ++moduleIt)
		{
			const YAML::Node& module(*moduleIt);
			modules.push_back(registrar.createFromYAML(module));
			//modules.push_back(createModuleFromRegistrar(module, registrar));
		}
	}
}

template<typename T>
template<typename R>
void PointMatcher<T>::ICPChainBase::createModuleFromRegistrar(const std::string& regName, const YAML::Node& doc, const R& registrar, std::shared_ptr<typename R::TargetType>& module)
{
	const YAML::Node *reg = doc.FindValue(regName);
	if (reg)
	{
		cout << regName << endl;
		//module.reset(createModuleFromRegistrar(*reg, registrar));
		module.reset(registrar.createFromYAML(*reg));
	}
	else
		module.reset();
}
/*
template<typename T>
template<typename R>
typename R::TargetType* PointMatcher<T>::ICPChainBase::createModuleFromRegistrar( const YAML::Node& module, const R& registrar)
{
	Parameters params;
	string name;
	
	if (module.size() != 1)
	{
		// parameter-less entry
		name = module.to<string>();
		cout << "  " << name << endl;
	}
	else
	{
		// get parameters
		YAML::Iterator mapIt(module.begin());
		mapIt.first() >> name;
		cout << "  " << name << endl;
		for(YAML::Iterator paramIt = mapIt.second().begin(); paramIt != mapIt.second().end(); ++paramIt)
		{
			std::string key, value;
			paramIt.first() >> key;
			paramIt.second() >> value;
			cout << "    " << key << ": " << value << endl;
			params[key] = value;
		}
	}
	
	return registrar.create(name, params);
}*/

#endif // HAVE_YAML_CPP

//! Perform ICP and return optimised transformation matrix
template<typename T>
typename PointMatcher<T>::TransformationParameters PointMatcher<T>::ICP::operator ()(
	const DataPoints& readingIn,
	const DataPoints& referenceIn)
{
	const int dim = readingIn.features.rows();
	TransformationParameters identity = TransformationParameters::Identity(dim, dim);
	return this->compute(readingIn, referenceIn, identity);
}

//! Perform ICP from initial guess and return optimised transformation matrix
template<typename T>
typename PointMatcher<T>::TransformationParameters PointMatcher<T>::ICP::operator ()(
	const DataPoints& readingIn,
	const DataPoints& referenceIn,
	const TransformationParameters& initialTransformationParameters)
{
	return this->compute(readingIn, referenceIn, initialTransformationParameters);
}

//! Perform ICP from initial guess and return optimised transformation matrix
template<typename T>
typename PointMatcher<T>::TransformationParameters PointMatcher<T>::ICP::compute(
	const DataPoints& readingIn,
	const DataPoints& referenceIn,
	const TransformationParameters& T_refIn_dataIn)
{
	// Ensuring minimum definition of components
	if (!this->matcher)
		throw runtime_error("You must setup a matcher before running ICP");
	if (!this->errorMinimizer)
		throw runtime_error("You must setup an error minimizer before running ICP");
	if (!this->inspector)
		throw runtime_error("You must setup an inspector before running ICP");
	if (!this->inspector)
		throw runtime_error("You must setup a logger before running ICP");
	
	this->inspector->init();
	
	timer t; // Print how long take the algo
	const int dim = referenceIn.features.rows();
	
	// Apply reference filters
	// reference is express in frame <refIn>
	DataPoints reference(referenceIn);
	this->keyframeDataPointsFilters.init();
	this->keyframeDataPointsFilters.apply(reference);
	
	// Create intermediate frame at the center of mass of reference pts cloud
	//  this help to solve for rotations
	const int nbPtsReference = referenceIn.features.cols();
	const Vector meanReference = referenceIn.features.rowwise().sum() / nbPtsReference;
	TransformationParameters T_refIn_refMean(Matrix::Identity(dim, dim));
	T_refIn_refMean.block(0,dim-1, dim-1, 1) = meanReference.head(dim-1);
	
	// Reajust reference position: 
	// from here reference is express in frame <refMean>
	// Shortcut to do T_refIn_refMean.inverse() * reference
	reference.features.topRows(dim-1).colwise() -= meanReference.head(dim-1);
	
	// Init matcher with reference points center on its mean
	this->matcher->init(reference);

	// Apply readings filters
	// reading is express in frame <dataIn>
	DataPoints reading(readingIn);
	const int nbPtsReading = reading.features.cols();
	this->readingDataPointsFilters.init();
	this->readingDataPointsFilters.apply(reading);
	
	// Reajust reading position: 
	// from here reading is express in frame <refMean>
	TransformationParameters 
		T_refMean_dataIn = T_refIn_refMean.inverse() * T_refIn_dataIn;
	this->transformations.apply(reading, T_refMean_dataIn);
	
	// Prepare reading filters used in the loop 
	this->readingStepDataPointsFilters.init();
	
	// Since reading and reference are express in <refMean>
	// the frame <refMean> is equivalent to the frame <iter(0)>
	TransformationParameters T_iter = Matrix::Identity(dim, dim);
	
	bool iterate(true);
	this->transformationCheckers.init(T_iter, iterate);

	size_t iterationCount(0);
	
	this->inspector->addStat("PreprocessingDuration", t.elapsed());
	this->inspector->addStat("PointCountReadingIn", readingIn.features.cols());
	this->inspector->addStat("PointCountReading", reading.features.cols());
	this->inspector->addStat("PointCountReferenceIn", referenceIn.features.cols());
	this->inspector->addStat("PointCountReference", reference.features.cols());
	
	LOG_INFO_STREAM("PointMatcher::icp - preprocess took " << t.elapsed() << " [s]");
	this->prefilteredKeyframePtsCount = reference.features.cols();
	LOG_INFO_STREAM("PointMatcher::icp - nb points in reference: " << nbPtsReference << " -> " << this->prefilteredKeyframePtsCount);
	this->prefilteredReadingPtsCount = reading.features.cols();
	LOG_INFO_STREAM( "PointMatcher::icp - nb points in reading: " << nbPtsReading << " -> " << this->prefilteredReadingPtsCount);
	t.restart();
	
	while (iterate)
	{
		DataPoints stepReading(reading);
		
		//-----------------------------
		// Apply step filter
		this->readingStepDataPointsFilters.apply(stepReading);
		
		//-----------------------------
		// Transform Readings
		this->transformations.apply(stepReading, T_iter);
		
		//-----------------------------
		// Match to closest point in Reference
		const Matches matches(
			this->matcher->findClosests(stepReading, reference)
		);
		
		//-----------------------------
		// Detect outliers
		const OutlierWeights outlierWeights(
			this->outlierFilters.compute(stepReading, reference, matches)
		);
		
		assert(outlierWeights.rows() == matches.ids.rows());
		assert(outlierWeights.cols() == matches.ids.cols());
		
		//cout << "outlierWeights: " << outlierWeights << "\n";
	
		
		//-----------------------------
		// Dump
		this->inspector->dumpIteration(
			iterationCount, T_iter, reference, stepReading, matches, outlierWeights, this->transformationCheckers
		);
		
		//-----------------------------
		// Error minimization
		// equivalent to: 
		//   T_iter(i+1)_iter(0) = T_iter(i+1)_iter(i) * T_iter(i)_iter(0)
		T_iter = this->errorMinimizer->compute(
			stepReading, reference, outlierWeights, matches) * T_iter;
		
		// Old version
		//T_iter = T_iter * this->errorMinimizer->compute(
		//	stepReading, reference, outlierWeights, matches);
		
		// in test
		
		this->transformationCheckers.check(T_iter, iterate);
	
		++iterationCount;
	}
	
	this->inspector->addStat("IterationsCount", iterationCount);
	this->inspector->addStat("PointCountTouched", this->matcher->getVisitCount());
	this->matcher->resetVisitCount();
	this->inspector->addStat("OverlapRatio", this->errorMinimizer->getWeightedPointUsedRatio());
	this->inspector->addStat("ConvergenceDuration", t.elapsed());
	this->inspector->finish(iterationCount);
	
	LOG_INFO_STREAM("PointMatcher::icp - " << iterationCount << " iterations took " << t.elapsed() << " [s]");
	
	// Move transformation back to original coordinate (without center of mass)
	// T_iter is equivalent to: T_iter(i+1)_iter(0)
	// the frame <iter(0)> equals <refMean>
	// so we have: 
	//   T_iter(i+1)_dataIn = T_iter(i+1)_iter(0) * T_refMean_dataIn
	//   T_iter(i+1)_dataIn = T_iter(i+1)_iter(0) * T_iter(0)_dataIn
	// T_refIn_refMean remove the temperary frame added during initialization
	return (T_refIn_refMean * T_iter * T_refMean_dataIn);
}

//! Construct a ICP sequence with ratioToSwitchKeyframe to 0.8
template<typename T>
PointMatcher<T>::ICPSequence::ICPSequence():
	ratioToSwitchKeyframe(0.8),
	keyFrameCreated(false)
{
}

//! destructor
template<typename T>
PointMatcher<T>::ICPSequence::~ICPSequence()
{
}

//! Return the latest transform
template<typename T>
typename PointMatcher<T>::TransformationParameters PointMatcher<T>::ICPSequence::getTransform() const
{
	return keyFrameTransform * T_refIn_dataIn;
}

//! Return the difference between the latest transform and the previous one
template<typename T>
typename PointMatcher<T>::TransformationParameters PointMatcher<T>::ICPSequence::getDeltaTransform() const
{
	return lastTransformInv * getTransform();
}

//! Return whether a keyframe was create at last () call
template<typename T>
bool PointMatcher<T>::ICPSequence::keyFrameCreatedAtLastCall() const
{
	return keyFrameCreated;
}

//! Return whether the object currently holds a keyframe
template<typename T>
bool PointMatcher<T>::ICPSequence::hasKeyFrame() const
{
	return (keyFrameCloud.features.cols() != 0);
}

//! Drop current key frame, create a new one with inputCloud, reset transformations
template<typename T>
void PointMatcher<T>::ICPSequence::resetTracking(DataPoints& inputCloud)
{
	const int tDim(keyFrameTransform.rows());
	createKeyFrame(inputCloud);
	keyFrameTransform = Matrix::Identity(tDim, tDim);
	lastTransformInv = Matrix::Identity(tDim, tDim);
}

//! Set default working values
template<typename T>
void PointMatcher<T>::ICPSequence::setDefault()
{
	ICPChainBase::setDefault();
	ratioToSwitchKeyframe = 0.8;
}

#ifdef HAVE_YAML_CPP
template<typename T>
void PointMatcher<T>::ICPSequence::loadAdditionalYAMLContent(YAML::Node& doc)
{
	if (doc.FindValue("ratioToSwitchKeyframe"))
		ratioToSwitchKeyframe = doc["ratioToSwitchKeyframe"].to<typeof(ratioToSwitchKeyframe)>();
}
#endif // HAVE_YAML_CPP

//! Create a new key frame
template<typename T>
void PointMatcher<T>::ICPSequence::createKeyFrame(DataPoints& inputCloud)
{
	timer t; // Print how long take the algo
	t.restart();
	const int dim(keyFrameTransform.rows());
	const int ptCount(inputCloud.features.cols());
	
	// update keyframe
	if (ptCount > 0)
	{
		// Apply reference filters
		// reference is express in frame <refIn>
		this->keyframeDataPointsFilters.init();
		this->keyframeDataPointsFilters.apply(inputCloud);
		
		this->inspector->addStat("PointCountKeyFrame", inputCloud.features.cols());

		// Create intermediate frame at the center of mass of reference pts cloud
		//  this help to solve for rotations
		const int nbPtsKeyframe = inputCloud.features.cols();
		const Vector meanKeyframe = inputCloud.features.rowwise().sum() / nbPtsKeyframe;
		T_refIn_refMean.block(0,dim-1, dim-1, 1) = meanKeyframe.head(dim-1);
		
		// Reajust reference position (only translations): 
		// from here reference is express in frame <refMean>
		// Shortcut to do T_refIn_refMean.inverse() * reference
		inputCloud.features.topRows(dim-1).colwise() -= meanKeyframe.head(dim-1);
	
		keyFrameCloud = inputCloud;
		T_refIn_dataIn = Matrix::Identity(dim, dim);
		
		this->matcher->init(keyFrameCloud);
		
		keyFrameCreated = true;
	
		this->inspector->addStat("KeyFrameDuration", t.elapsed());
	}
	else
		LOG_WARNING_STREAM("Warning: ignoring attempt to create a keyframe from an empty cloud (" << ptCount << " points before filtering)");
}

//! Apply ICP to cloud inputCloudIn, create a new keyframe if none is present or if the ratio of matching points is below ratioToSwitchKeyframe.
template<typename T>
typename PointMatcher<T>::TransformationParameters PointMatcher<T>::ICPSequence::operator ()(
	const DataPoints& inputCloudIn)
{
	// Ensuring minimum definition of components
	if (!this->matcher)
		throw runtime_error("You must setup a matcher before running ICP");
	if (!this->errorMinimizer)
		throw runtime_error("You must setup an error minimizer before running ICP");
	if (!this->inspector)
		throw runtime_error("You must setup an inspector before running ICP");
	if (!this->inspector)
		throw runtime_error("You must setup a logger before running ICP");
	
	lastTransformInv = getTransform().inverse();
	DataPoints inputCloud(inputCloudIn);
	
	const int dim = inputCloudIn.features.rows();
	
	this->inspector->init();
	
	// initial keyframe
	keyFrameCreated = false;
	if (!hasKeyFrame())
	{
		timer t;
		const int ptCount = inputCloudIn.features.cols();
		
		if(!(dim == 3 || dim == 4))
			throw runtime_error("Point cloud should be 2D or 3D in homogeneous coordinates");
		if (ptCount == 0)
			return Matrix::Identity(dim, dim);
		
		// Initialize transformation matrices
		keyFrameTransform = Matrix::Identity(dim, dim);
		T_refIn_refMean = Matrix::Identity(dim, dim);
		T_refIn_dataIn = Matrix::Identity(dim, dim);
		lastTransformInv = Matrix::Identity(dim, dim);

		this->createKeyFrame(inputCloud);
		
		this->inspector->addStat("KeyframingDuration", t.elapsed());
		
		return T_refIn_dataIn;
	}
	else
	{
		if(inputCloudIn.features.rows() != keyFrameTransform.rows())
			throw runtime_error((boost::format("Point cloud should not change dimensions. Homogeneous dimension was %1% and is now %2%.") % keyFrameTransform.rows() % inputCloudIn.features.rows()).str());
	}
	
	timer t; // Print how long take the algo
	
	// Apply readings filters
	// reading is express in frame <dataIn>
	DataPoints reading(inputCloud);
	this->readingDataPointsFilters.init();
	this->readingDataPointsFilters.apply(reading);
	
	this->inspector->addStat("PreprocessingDuration", t.elapsed());
	this->inspector->addStat("PointCountIn", inputCloud.features.cols());
	this->inspector->addStat("PointCountReading", reading.features.cols());
	t.restart();
	
	// Reajust reading position: 
	// from here reading is express in frame <refMean>
	TransformationParameters
		T_refMean_dataIn = T_refIn_refMean.inverse() * T_refIn_dataIn;
	this->transformations.apply(reading, T_refMean_dataIn);

	//cout << "T_refMean_dataIn: " << endl << T_refMean_dataIn << endl;
	// Prepare reading filters used in the loop 
	this->readingStepDataPointsFilters.init();
	
	// Since reading and reference are express in <refMean>
	// the frame <refMean> is equivalent to the frame <iter(0)>
	TransformationParameters T_iter = Matrix::Identity(dim, dim);
	
	bool iterate(true);
	this->transformationCheckers.init(T_iter, iterate);
	
	size_t iterationCount(0);

	while (iterate)
	{
		DataPoints stepReading(reading);
		
		//-----------------------------
		// Apply step filter
		this->readingStepDataPointsFilters.apply(stepReading);
		
		//-----------------------------
		// Transform Readings
		this->transformations.apply(stepReading, T_iter);
		
		//-----------------------------
		// Match to closest point in Reference
		const Matches matches(
			this->matcher->findClosests(stepReading, keyFrameCloud)
		);
		
		//-----------------------------
		// Detect outliers
		//cout << matches.ids.leftCols(10) << endl;
		//cout << matches.dists.leftCols(10) << endl;
		const OutlierWeights outlierWeights(
			this->outlierFilters.compute(stepReading, keyFrameCloud, matches)
		);
		

		assert(outlierWeights.rows() == matches.ids.rows());
		assert(outlierWeights.cols() == matches.ids.cols());
		
		//-----------------------------
		// Dump
		this->inspector->dumpIteration(
			iterationCount, T_iter, keyFrameCloud, stepReading, matches, outlierWeights, this->transformationCheckers
		);
		
		//-----------------------------
		// Error minimization
		// equivalent to: 
		//   T_iter(i+1)_iter(0) = T_iter(i+1)_iter(i) * T_iter(i)_iter(0)
		T_iter = this->errorMinimizer->compute(
			stepReading, keyFrameCloud, outlierWeights, matches) * T_iter;
		
		this->transformationCheckers.check(T_iter, iterate);
		
		//cout << "T_iter: " << endl << T_iter << endl;

		++iterationCount;
	}
	
	this->inspector->addStat("IterationsCount", iterationCount);
	this->inspector->addStat("PointCountTouched", this->matcher->getVisitCount());
	this->matcher->resetVisitCount();
	this->inspector->addStat("OverlapRatio", this->errorMinimizer->getWeightedPointUsedRatio());
	this->inspector->addStat("ConvergenceDuration", t.elapsed());
	t.restart();
	
	// Move transformation back to original coordinate (without center of mass)
	// T_iter is equivalent to: T_iter(i+1)_iter(0)
	// the frame <iter(0)> equals <refMean>
	// so we have: 
	//   T_iter(i+1)_dataIn = T_iter(i+1)_iter(0) * T_refMean_dataIn
	//   T_iter(i+1)_dataIn = T_iter(i+1)_iter(0) * T_iter(0)_dataIn
	// T_refIn_refMean remove the temperary frame added during initialization
	T_refIn_dataIn = T_refIn_refMean * T_iter * T_refMean_dataIn;
	
	if (this->errorMinimizer->getWeightedPointUsedRatio() < ratioToSwitchKeyframe)
	{
		// new keyframe
		keyFrameTransform *= T_refIn_dataIn;
		this->createKeyFrame(inputCloud);
		this->inspector->addStat("KeyframingDuration", t.elapsed());
	}
	
	this->inspector->finish(iterationCount);
	
	//cout << "keyFrameTransform: " << endl << keyFrameTransform << endl;
	//cout << "T_refIn_dataIn: " << endl << T_refIn_dataIn << endl;
	// Return transform in world space
	return keyFrameTransform * T_refIn_dataIn;
}

//! Constructor, populates the registrars
template<typename T>
PointMatcher<T>::PointMatcher()
{
	ADD_TO_REGISTRAR_NO_PARAM(Transformation, RigidTransformation, typename TransformationsImpl<T>::RigidTransformation)
	
	ADD_TO_REGISTRAR_NO_PARAM(DataPointsFilter, IdentityDataPointsFilter, typename DataPointsFiltersImpl<T>::IdentityDataPointsFilter)
	ADD_TO_REGISTRAR_NO_PARAM(DataPointsFilter, RemoveNaNDataPointsFilter, typename DataPointsFiltersImpl<T>::RemoveNaNDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, MaxDistDataPointsFilter, typename DataPointsFiltersImpl<T>::MaxDistDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, MinDistDataPointsFilter, typename DataPointsFiltersImpl<T>::MinDistDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, MaxQuantileOnAxisDataPointsFilter, typename DataPointsFiltersImpl<T>::MaxQuantileOnAxisDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, MaxDensityDataPointsFilter, typename DataPointsFiltersImpl<T>::MaxDensityDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, SurfaceNormalDataPointsFilter, typename DataPointsFiltersImpl<T>::SurfaceNormalDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, SamplingSurfaceNormalDataPointsFilter, typename DataPointsFiltersImpl<T>::SamplingSurfaceNormalDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, OrientNormalsDataPointsFilter, typename DataPointsFiltersImpl<T>::OrientNormalsDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, RandomSamplingDataPointsFilter, typename DataPointsFiltersImpl<T>::RandomSamplingDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, FixStepSamplingDataPointsFilter, typename DataPointsFiltersImpl<T>::FixStepSamplingDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, ShadowDataPointsFilter, typename DataPointsFiltersImpl<T>::ShadowDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, SimpleSensorNoiseDataPointsFilter, typename DataPointsFiltersImpl<T>::SimpleSensorNoiseDataPointsFilter)
	ADD_TO_REGISTRAR(DataPointsFilter, ObservationDirectionDataPointsFilter, typename DataPointsFiltersImpl<T>::ObservationDirectionDataPointsFilter)
	
	ADD_TO_REGISTRAR_NO_PARAM(Matcher, NullMatcher, typename MatchersImpl<T>::NullMatcher)
	ADD_TO_REGISTRAR(Matcher, KDTreeMatcher, typename MatchersImpl<T>::KDTreeMatcher)
	ADD_TO_REGISTRAR(Matcher, KDTreeVarDistMatcher, typename MatchersImpl<T>::KDTreeVarDistMatcher)
	
	ADD_TO_REGISTRAR_NO_PARAM(OutlierFilter, NullOutlierFilter, typename OutlierFiltersImpl<T>::NullOutlierFilter)
	ADD_TO_REGISTRAR(OutlierFilter, MaxDistOutlierFilter, typename OutlierFiltersImpl<T>::MaxDistOutlierFilter)
	ADD_TO_REGISTRAR(OutlierFilter, MinDistOutlierFilter, typename OutlierFiltersImpl<T>::MinDistOutlierFilter)
	ADD_TO_REGISTRAR(OutlierFilter, MedianDistOutlierFilter, typename OutlierFiltersImpl<T>::MedianDistOutlierFilter)
	ADD_TO_REGISTRAR(OutlierFilter, TrimmedDistOutlierFilter, typename OutlierFiltersImpl<T>::TrimmedDistOutlierFilter)
	ADD_TO_REGISTRAR(OutlierFilter, VarTrimmedDistOutlierFilter, typename OutlierFiltersImpl<T>::VarTrimmedDistOutlierFilter)
	ADD_TO_REGISTRAR(OutlierFilter, SurfaceNormalOutlierFilter, typename OutlierFiltersImpl<T>::SurfaceNormalOutlierFilter)
	
	ADD_TO_REGISTRAR_NO_PARAM(ErrorMinimizer, IdentityErrorMinimizer, typename ErrorMinimizersImpl<T>::IdentityErrorMinimizer)
	ADD_TO_REGISTRAR_NO_PARAM(ErrorMinimizer, PointToPointErrorMinimizer, typename ErrorMinimizersImpl<T>::PointToPointErrorMinimizer)
	ADD_TO_REGISTRAR_NO_PARAM(ErrorMinimizer, PointToPlaneErrorMinimizer, typename ErrorMinimizersImpl<T>::PointToPlaneErrorMinimizer)
	
	ADD_TO_REGISTRAR(TransformationChecker, CounterTransformationChecker, typename TransformationCheckersImpl<T>::CounterTransformationChecker)
	ADD_TO_REGISTRAR(TransformationChecker, DifferentialTransformationChecker, typename TransformationCheckersImpl<T>::DifferentialTransformationChecker)
	ADD_TO_REGISTRAR(TransformationChecker, BoundTransformationChecker, typename TransformationCheckersImpl<T>::BoundTransformationChecker)
	
	ADD_TO_REGISTRAR_NO_PARAM(Inspector, NullInspector, typename InspectorsImpl<T>::NullInspector)
	ADD_TO_REGISTRAR(Inspector, PerformanceInspector, typename InspectorsImpl<T>::PerformanceInspector)
	ADD_TO_REGISTRAR(Inspector, VTKFileInspector, typename InspectorsImpl<T>::VTKFileInspector)
	
	ADD_TO_REGISTRAR_NO_PARAM(Logger, NullLogger, NullLogger)
	ADD_TO_REGISTRAR(Logger, FileLogger, FileLogger)
}

template struct PointMatcher<float>;
template struct PointMatcher<double>;

