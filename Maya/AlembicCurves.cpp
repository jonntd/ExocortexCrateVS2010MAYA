#include "stdafx.h"

#include "AlembicCurves.h"
#include "AttributesReading.h"
#include "MetaData.h"

#include <maya/MArrayDataBuilder.h>
#include <maya/MArrayDataHandle.h>
#include <maya/MPlug.h>
#include <map>

//-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
AlembicCurveAccumulator::AlembicCurveAccumulator(const MObject &ref,
                                                 SceneNodePtr eNode,
                                                 AlembicWriteJob *in_Job,
                                                 Abc::OObject oParent)
    : m_ref(ref),
      firstSample(true),
      useGlobalCache(in_Job->GetOption(L"exportInGlobalSpace").asInt() > 0)
{
  const bool animTS = (in_Job->GetAnimatedTs() > 0);
  mObject = AbcG::OCurves(oParent, eNode->name, animTS);
  mSchema = mObject.getSchema();

  // create all properties
  // Abc::OCompoundProperty comp = mSchema.getArbGeomParams();
  // mRadiusProperty = Abc::OFloatArrayProperty(comp, ".radius",
  // mSchema.getMetaData(), animTS );
  // mColorProperty = Abc::OC4fArrayProperty(comp, ".color",
  // mSchema.getMetaData(), animTS );
}

void AlembicCurveAccumulator::startRecording(void)
{
  mInverse = GetGlobalMatrix(m_ref).inverse();
  mPosVec.clear();
  mRadiusVec.clear();
  mKnotVec.clear();
  mNbVertices.clear();
  bbox.makeEmpty();
}
void AlembicCurveAccumulator::stopRecording(void)
{
  if (firstSample) {
    mSample.setCurvesNumVertices(Abc::Int32ArraySample(mNbVertices));
    mSample.setWrap(AbcG::kNonPeriodic);
    mSample.setType(AbcG::kLinear);

    // mRadiusProperty.set(Abc::FloatArraySample(&mRadiusVec.front(),mRadiusVec.size()));
    // mColorProperty.set(Abc::C4fArraySample(&mColorVec.front(),mColorVec.size()));
    firstSample = false;
  }
  mSample.setPositions(Abc::P3fArraySample(&mPosVec.front(), mPosVec.size()));
  mSample.setSelfBounds(bbox);
  mSchema.set(mSample);
}
void AlembicCurveAccumulator::save(const MObject &ref, double time)
{
  MFnNurbsCurve node(ref);

  Abc::M44f globalXfo = GetGlobalMatrix(ref);
  if (!this->useGlobalCache) {
    Abc::M44f::multiply(globalXfo, this->mInverse, globalXfo);
  }

  MPointArray positions;
  node.getCVs(positions);

  int offset = (int)mPosVec.size();
  const int posLen = positions.length();
  mPosVec.resize(offset + posLen);
  for (unsigned int i = 0; i < (unsigned int)posLen; ++i, ++offset) {
    const MPoint &outPos = positions[i];
    Imath::V3f &inPos = mPosVec[offset];
    inPos.x = (float)outPos.x;
    inPos.y = (float)outPos.y;
    inPos.z = (float)outPos.z;
    globalXfo.multVecMatrix(inPos, inPos);
    bbox.extendBy(inPos);
  }

  if (firstSample) {
    this->mNbVertices.push_back(posLen);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// - - - - - - - - - - - - - - - - - - - -

typedef std::map<int, AlembicCurveAccumulatorPtr> mapIntAccumulator;
typedef mapIntAccumulator::iterator accumIter;
static mapIntAccumulator accumulators;

void AlembicCurveAccumulator::Initialize(void) { accumulators.clear(); }
void AlembicCurveAccumulator::StartRecordingFrame(void)
{
  for (accumIter first = accumulators.begin(), last = accumulators.end();
       first != last; ++first) {
    first->second->startRecording();
  }
}
void AlembicCurveAccumulator::StopRecordingFrame(void)
{
  for (accumIter first = accumulators.begin(), last = accumulators.end();
       first != last; ++first) {
    first->second->stopRecording();
  }
}
void AlembicCurveAccumulator::Destroy(void) { accumulators.clear(); }
AlembicCurveAccumulatorPtr AlembicCurveAccumulator::GetAccumulator(
    int id, const MObject &ref, SceneNodePtr eNode, AlembicWriteJob *in_Job,
    Abc::OObject oParent)
{
  if (accumulators.find(id) == accumulators.end())
    accumulators[id] = AlembicCurveAccumulatorPtr(
        new AlembicCurveAccumulator(ref, eNode, in_Job, oParent));
  return accumulators[id];
}

//-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
AlembicCurves::AlembicCurves(SceneNodePtr eNode, AlembicWriteJob *in_Job,
                             Abc::OObject oParent)
    : AlembicObject(eNode, in_Job, oParent)
{
  MObject nRef = GetRef();
  MFnNurbsCurve node(nRef);
  MStatus status;
  MObject abcCurveId = node.attribute("abcCurveId", &status);
  if (status == MStatus::kSuccess) {
    const int cId = MPlug(nRef, abcCurveId).asInt();
    if (cId != 0) {
      this->accumRef = AlembicCurveAccumulator::GetAccumulator(cId, nRef, eNode,
                                                               in_Job, oParent);
      return;
    }
  }

  const bool animTS = (GetJob()->GetAnimatedTs() > 0);
  mObject = AbcG::OCurves(GetMyParent(), eNode->name, animTS);
  mSchema = mObject.getSchema();

  // create all properties
  Abc::OCompoundProperty comp = mSchema.getArbGeomParams();
  mRadiusProperty =
      Abc::OFloatArrayProperty(comp, ".radius", mSchema.getMetaData(), animTS);
  mColorProperty =
      Abc::OC4fArrayProperty(comp, ".color", mSchema.getMetaData(), animTS);
  mFaceIndexProperty = Abc::OInt32ArrayProperty(comp, ".face_index",
                                                mSchema.getMetaData(), animTS);
  mVertexIndexProperty = Abc::OInt32ArrayProperty(
      comp, ".vertex_index", mSchema.getMetaData(), animTS);
  mKnotVectorProperty = Abc::OFloatArrayProperty(comp, ".knot_vector",
                                                 mSchema.getMetaData(), animTS);
}

AlembicCurves::~AlembicCurves()
{
  if (this->accumRef.get() == 0) {
    mObject.reset();
    mSchema.reset();
  }
  else {
    this->accumRef.reset();
  }
}

MStatus AlembicCurves::Save(double time, unsigned int timeIndex,
    bool isFirstFrame)
{
  ESS_PROFILE_SCOPE("AlembicCurves::Save");

  if (this->accumRef.get() != 0) {
    accumRef->save(GetRef(), time);
    ++mNumSamples;
    return MStatus::kSuccess;
  }

  // access the geometry
  MFnNurbsCurve node(GetRef());

  // save the metadata
  SaveMetaData(this);

  // save the attributes
  if (isFirstFrame) {
    Abc::OCompoundProperty cp;
    Abc::OCompoundProperty up;
    if (AttributesWriter::hasAnyAttr(node, *GetJob())) {
      cp = mSchema.getArbGeomParams();
      up = mSchema.getUserProperties();
    }

    mAttrs = AttributesWriterPtr(
        new AttributesWriter(cp, up, GetMyParent(), node, timeIndex, *GetJob())
        );
  }
  else {
    mAttrs->write();
  }

  // prepare the bounding box
  Abc::Box3d bbox;

  // check if we have the global cache option
  const bool globalCache =
      GetJob()->GetOption(L"exportInGlobalSpace").asInt() > 0;
  Abc::M44f globalXfo;
  if (globalCache) {
    globalXfo = GetGlobalMatrix(GetRef());
  }

  MPointArray positions;
  node.getCVs(positions);

  mPosVec.resize(positions.length());
  for (unsigned int i = 0; i < positions.length(); i++) {
    const MPoint &outPos = positions[i];
    Imath::V3f &inPos = mPosVec[i];
    inPos.x = (float)outPos.x;
    inPos.y = (float)outPos.y;
    inPos.z = (float)outPos.z;
    if (globalCache) {
      globalXfo.multVecMatrix(inPos, inPos);
    }
    bbox.extendBy(inPos);
  }

  // store the positions to the samples
  mSample.setPositions(Abc::P3fArraySample(&mPosVec.front(), mPosVec.size()));
  mSample.setSelfBounds(bbox);

  if (mNumSamples == 0) {
    // knot vector!
    MDoubleArray knots;
    node.getKnots(knots);

    mKnotVec.resize(knots.length());
    for (unsigned int i = 0; i < knots.length(); ++i) {
      mKnotVec[i] = (float)knots[i];
    }

    mKnotVectorProperty.set(Abc::FloatArraySample(mKnotVec));

    mNbVertices.push_back(node.numCVs());
    mSample.setCurvesNumVertices(Abc::Int32ArraySample(mNbVertices));

    if (node.form() == MFnNurbsCurve::kOpen) {
      mSample.setWrap(AbcG::kNonPeriodic);
    }
    else {
      mSample.setWrap(AbcG::kPeriodic);
    }

    if (node.degree() == 3) {
      mSample.setType(AbcG::kCubic);
    }
    else {
      mSample.setType(AbcG::kLinear);
    }

    MPlug widthPlug = node.findPlug("width");
    if (!widthPlug.isNull()) {
      mRadiusVec.push_back(widthPlug.asFloat());
    }
    else {
      mRadiusVec.push_back(1.0);
    }

    mRadiusProperty.set(
        Abc::FloatArraySample(&mRadiusVec.front(), mRadiusVec.size()));
  }

  // save the sample
  mSchema.set(mSample);
  mNumSamples++;

  return MStatus::kSuccess;
}

void AlembicCurvesNode::PreDestruction()
{
  mSchema.reset();
  delRefArchive(mFileName);
  mFileName.clear();
}

AlembicCurvesNode::~AlembicCurvesNode() { PreDestruction(); }
MObject AlembicCurvesNode::mTimeAttr;
MObject AlembicCurvesNode::mFileNameAttr;
MObject AlembicCurvesNode::mIdentifierAttr;
MObject AlembicCurvesNode::mOutGeometryAttr;

MObject AlembicCurvesNode::mGeomParamsList;
MObject AlembicCurvesNode::mUserAttrsList;

MStatus AlembicCurvesNode::initialize()
{
  MStatus status;

  MFnUnitAttribute uAttr;
  MFnTypedAttribute tAttr;
  MFnNumericAttribute nAttr;
  MFnGenericAttribute gAttr;
  MFnStringData emptyStringData;
  MObject emptyStringObject = emptyStringData.create("");

  // input time
  mTimeAttr = uAttr.create("inTime", "tm", MFnUnitAttribute::kTime, 0.0);
  status = uAttr.setStorable(true);
  status = uAttr.setKeyable(true);
  status = addAttribute(mTimeAttr);

  // input file name
  mFileNameAttr =
      tAttr.create("fileName", "fn", MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setUsedAsFilename(true);
  status = tAttr.setKeyable(false);
  status = addAttribute(mFileNameAttr);

  // input identifier
  mIdentifierAttr =
      tAttr.create("identifier", "if", MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = addAttribute(mIdentifierAttr);

  // output curve
  mOutGeometryAttr = tAttr.create("outCurve", "os", MFnData::kNurbsCurve);
  status = tAttr.setArray(true);
  status = tAttr.setReadable(true);
  status = tAttr.setUsesArrayDataBuilder(true);
  status = tAttr.setStorable(false);
  status = tAttr.setWritable(false);
  status = tAttr.setKeyable(false);
  status = tAttr.setHidden(false);
  status = addAttribute(mOutGeometryAttr);

  // output for list of ArbGeomParams
  mGeomParamsList = tAttr.create("ExocortexAlembic_GeomParams", "exo_gp",
      MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = tAttr.setHidden(false);
  status = tAttr.setInternal(true);
  status = addAttribute(mGeomParamsList);

  // output for list of UserAttributes
  mUserAttrsList = tAttr.create("ExocortexAlembic_UserAttributes", "exo_ua",
      MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = tAttr.setHidden(false);
  status = tAttr.setInternal(true);
  status = addAttribute(mUserAttrsList);

  // create a mapping
  status = attributeAffects(mTimeAttr, mOutGeometryAttr);
  status = attributeAffects(mFileNameAttr, mOutGeometryAttr);
  status = attributeAffects(mIdentifierAttr, mOutGeometryAttr);
  return status;
}

MStatus AlembicCurvesNode::compute(const MPlug &plug, MDataBlock &dataBlock)
{
  ESS_PROFILE_SCOPE("AlembicCurvesNode::compute");
  MStatus status;

  // update the frame number to be imported
  const double inputTime =
      dataBlock.inputValue(mTimeAttr).asTime().as(MTime::kSeconds);
  MString &fileName = dataBlock.inputValue(mFileNameAttr).asString();
  MString &identifier = dataBlock.inputValue(mIdentifierAttr).asString();

  // check if we have the file
  if (fileName != mFileName || identifier != mIdentifier) {
    mSchema.reset();
    if (fileName != mFileName) {
      delRefArchive(mFileName);
      mFileName = fileName;
      addRefArchive(mFileName);
    }
    mIdentifier = identifier;

    // get the object from the archive
    Abc::IObject iObj = getObjectFromArchive(mFileName, identifier);
    if (!iObj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' not found in archive '" + mFileName + "'.");
      return MStatus::kFailure;
    }
    AbcG::ICurves obj(iObj, Abc::kWrapExisting);
    if (!obj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' in archive '" + mFileName +
                              "' is not a Curves.");
      return MStatus::kFailure;
    }
    mObj = obj;
    mSchema = obj.getSchema();
    mCurvesData = MObject::kNullObj;
  }

  if (!mSchema.valid()) {
    return MStatus::kFailure;
  }

  {
    ESS_PROFILE_SCOPE("AlembicCurvesNode::compute readProps");
    Alembic::Abc::ICompoundProperty arbProp = mSchema.getArbGeomParams();
    Alembic::Abc::ICompoundProperty userProp = mSchema.getUserProperties();
    readProps(inputTime, arbProp, dataBlock, thisMObject());
    readProps(inputTime, userProp, dataBlock, thisMObject());

    // Set all plugs as clean
    // Even if one of them failed to get set,
    // trying again in this frame isn't going to help
    for (unsigned int i = 0; i < mGeomParamPlugs.length(); i++) {
      dataBlock.outputValue(mGeomParamPlugs[i]).setClean();
    }

    for (unsigned int i = 0; i < mUserAttrPlugs.length(); i++) {
      dataBlock.outputValue(mUserAttrPlugs[i]).setClean();
    }
  }

  // get the sample
  SampleInfo sampleInfo = getSampleInfo(inputTime, mSchema.getTimeSampling(),
                                        mSchema.getNumSamples());

  // check if we have to do this at all
  if (!mCurvesData.isNull() &&
      mLastSampleInfo.floorIndex == sampleInfo.floorIndex &&
      mLastSampleInfo.ceilIndex == sampleInfo.ceilIndex) {
    return MStatus::kSuccess;
  }

  mLastSampleInfo = sampleInfo;
  const float blend = (float)sampleInfo.alpha;

  // access the camera values
  AbcG::ICurvesSchema::Sample sample;
  AbcG::ICurvesSchema::Sample sample2;
  mSchema.get(sample, sampleInfo.floorIndex);
  if (blend != 0.0f) {
    mSchema.get(sample2, sampleInfo.ceilIndex);
  }

  Abc::P3fArraySamplePtr samplePos = sample.getPositions();
  Abc::P3fArraySamplePtr samplePos2 = sample2.getPositions();
  Abc::Int32ArraySamplePtr nbVertices = sample.getCurvesNumVertices();
  const bool applyBlending =
      (blend == 0.0f) ? false : (samplePos->size() == samplePos2->size());

  Abc::FloatArraySamplePtr pKnotVec = getKnotVector(mObj);
  Abc::UInt16ArraySamplePtr pOrders = getCurveOrders(mObj);

  MArrayDataHandle arrh = dataBlock.outputArrayValue(mOutGeometryAttr);
  MArrayDataBuilder builder = arrh.builder();

  // reference:
  // http://download.autodesk.com/us/maya/2010help/API/multi_curve_node_8cpp-example.html

  const int degree = (sample.getType() == AbcG::kCubic) ? 3 : 1;
  const bool closed = (sample.getWrap() == AbcG::kPeriodic);
  unsigned int pointOffset = 0;
  unsigned int knotOffset = 0;
  for (int ii = 0; ii < nbVertices->size(); ++ii) {
    const unsigned int nbCVs = (unsigned int)nbVertices->get()[ii];
    const int ldegree = (pOrders) ? pOrders->get()[ii] : degree;
    const int nbSpans = (int)nbCVs - ldegree;

    MDoubleArray knots;
    if (pKnotVec) {
      const unsigned int nb_knot = nbCVs + ldegree - 1;
      for (unsigned int i = 0; i < nb_knot; ++i) {
        knots.append(pKnotVec->get()[knotOffset + i]);
      }
      knotOffset += nb_knot;
    }
    else {
      for (int span = 0; span <= nbSpans; ++span) {
        knots.append(double(span));
        if (span == 0 || span == nbSpans) {
          for (int m = 1; m < degree; ++m) {
            knots.append(double(span));
          }
        }
      }
    }

    MPointArray points;
    if (samplePos->size() > 0) {
      points.setLength((unsigned int)nbCVs);
      if (applyBlending) {
        for (unsigned int i = 0; i < nbCVs; ++i) {
          const Abc::P3fArraySample::value_type &vals1 =
              samplePos->get()[pointOffset + i];
          const Abc::P3fArraySample::value_type &vals2 =
              samplePos2->get()[pointOffset + i];
          MPoint &pt = points[i];

          pt.x = vals1.x + (vals2.x - vals1.x) * blend;
          pt.y = vals1.y + (vals2.y - vals1.y) * blend;
          pt.z = vals1.z + (vals2.z - vals1.z) * blend;
        }
      }
      else {
        for (unsigned int i = 0; i < nbCVs; ++i) {
          const Abc::P3fArraySample::value_type &vals =
              samplePos->get()[pointOffset + i];
          MPoint &pt = points[i];
          pt.x = vals.x;
          pt.y = vals.y;
          pt.z = vals.z;
        }
      }
      pointOffset += nbCVs;
    }

    // create a subd either with or without uvs
    MObject mmCurvesData = MFnNurbsCurveData().create();
    if (ldegree == 1 || ldegree == 3)
      mCurves.create(points, knots, ldegree,
                     closed ? MFnNurbsCurve::kClosed : MFnNurbsCurve::kOpen,
                     false, false, mmCurvesData);
    builder.addElement(ii).set(mmCurvesData);
  }
  arrh.set(builder);
  arrh.setAllClean();
  return MStatus::kSuccess;
}

// Cache the plug arrays for use in setDependentsDirty
bool AlembicCurvesNode::setInternalValueInContext(const MPlug & plug,
    const MDataHandle & dataHandle,
    MDGContext &)
{
  if (plug == mGeomParamsList) {
    MString geomParamsStr = dataHandle.asString();
    getPlugArrayFromAttrList(geomParamsStr, thisMObject(), mGeomParamPlugs);
  }
  else if (plug == mUserAttrsList) {
    MString userAttrsStr = dataHandle.asString();
    getPlugArrayFromAttrList(userAttrsStr, thisMObject(), mUserAttrPlugs);
  }

  return false;
}

MStatus AlembicCurvesNode::setDependentsDirty(const MPlug &plugBeingDirtied,
    MPlugArray &affectedPlugs)
{
  if (plugBeingDirtied == mFileNameAttr || plugBeingDirtied == mIdentifierAttr
      || plugBeingDirtied == mTimeAttr) {

    for (unsigned int i = 0; i < mGeomParamPlugs.length(); i++) {
      affectedPlugs.append(mGeomParamPlugs[i]);
    }

    for (unsigned int i = 0; i < mUserAttrPlugs.length(); i++) {
      affectedPlugs.append(mUserAttrPlugs[i]);
    }
  }

  return MStatus::kSuccess;
}

void AlembicCurvesDeformNode::PreDestruction()
{
  mSchema.reset();
  delRefArchive(mFileName);
  mFileName.clear();
}

AlembicCurvesDeformNode::~AlembicCurvesDeformNode() { PreDestruction(); }
MObject AlembicCurvesDeformNode::mTimeAttr;
MObject AlembicCurvesDeformNode::mFileNameAttr;
MObject AlembicCurvesDeformNode::mIdentifierAttr;

MObject AlembicCurvesDeformNode::mGeomParamsList;
MObject AlembicCurvesDeformNode::mUserAttrsList;

MStatus AlembicCurvesDeformNode::initialize()
{
  MStatus status;

  MFnUnitAttribute uAttr;
  MFnTypedAttribute tAttr;
  MFnNumericAttribute nAttr;
  MFnGenericAttribute gAttr;
  MFnStringData emptyStringData;
  MObject emptyStringObject = emptyStringData.create("");

  // input time
  mTimeAttr = uAttr.create("inTime", "tm", MFnUnitAttribute::kTime, 0.0);
  status = uAttr.setStorable(true);
  status = uAttr.setKeyable(true);
  status = addAttribute(mTimeAttr);

  // input file name
  mFileNameAttr =
      tAttr.create("fileName", "fn", MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setUsedAsFilename(true);
  status = tAttr.setKeyable(false);
  status = addAttribute(mFileNameAttr);

  // input identifier
  mIdentifierAttr =
      tAttr.create("identifier", "if", MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = addAttribute(mIdentifierAttr);

  // output for list of ArbGeomParams
  mGeomParamsList = tAttr.create("ExocortexAlembic_GeomParams", "exo_gp",
      MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = tAttr.setHidden(false);
  status = tAttr.setInternal(true);
  status = addAttribute(mGeomParamsList);

  // output for list of UserAttributes
  mUserAttrsList = tAttr.create("ExocortexAlembic_UserAttributes", "exo_ua",
      MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = tAttr.setHidden(false);
  status = tAttr.setInternal(true);
  status = addAttribute(mUserAttrsList);

  // create a mapping
  status = attributeAffects(mTimeAttr, outputGeom);
  status = attributeAffects(mFileNameAttr, outputGeom);
  status = attributeAffects(mIdentifierAttr, outputGeom);

  return status;
}

MStatus AlembicCurvesDeformNode::deform(MDataBlock &dataBlock,
                                        MItGeometry &iter,
                                        const MMatrix &localToWorld,
                                        unsigned int geomIndex)
{
  // get the envelope data
  float env = dataBlock.inputValue(envelope).asFloat();
  if (env == 0.0f) {  // deformer turned off
    return MStatus::kSuccess;
  }

  // update the frame number to be imported
  double inputTime =
      dataBlock.inputValue(mTimeAttr).asTime().as(MTime::kSeconds);
  MString &fileName = dataBlock.inputValue(mFileNameAttr).asString();
  MString &identifier = dataBlock.inputValue(mIdentifierAttr).asString();

  // check if we have the file
  if (fileName != mFileName || identifier != mIdentifier) {
    mSchema.reset();
    if (fileName != mFileName) {
      delRefArchive(mFileName);
      mFileName = fileName;
      addRefArchive(mFileName);
    }
    mIdentifier = identifier;

    // get the object from the archive
    Abc::IObject iObj = getObjectFromArchive(mFileName, identifier);
    if (!iObj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' not found in archive '" + mFileName + "'.");
      return MStatus::kFailure;
    }
    AbcG::ICurves obj(iObj, Abc::kWrapExisting);
    if (!obj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' in archive '" + mFileName +
                              "' is not a Curves.");
      return MStatus::kFailure;
    }
    mSchema = obj.getSchema();
  }

  if (!mSchema.valid()) {
    return MStatus::kFailure;
  }

  {
    ESS_PROFILE_SCOPE("AlembicCurvesDeformNode::deform readProps");
    Alembic::Abc::ICompoundProperty arbProp = mSchema.getArbGeomParams();
    Alembic::Abc::ICompoundProperty userProp = mSchema.getUserProperties();
    readProps(inputTime, arbProp, dataBlock, thisMObject());
    readProps(inputTime, userProp, dataBlock, thisMObject());

    // Set all plugs as clean
    // Even if one of them failed to get set,
    // trying again in this frame isn't going to help
    for (unsigned int i = 0; i < mGeomParamPlugs.length(); i++) {
      dataBlock.outputValue(mGeomParamPlugs[i]).setClean();
    }

    for (unsigned int i = 0; i < mUserAttrPlugs.length(); i++) {
      dataBlock.outputValue(mUserAttrPlugs[i]).setClean();
    }
  }

  // get the sample
  SampleInfo sampleInfo = getSampleInfo(inputTime, mSchema.getTimeSampling(),
                                        mSchema.getNumSamples());

  // check if we have to do this at all
  if (mLastSampleInfo.floorIndex == sampleInfo.floorIndex &&
      mLastSampleInfo.ceilIndex == sampleInfo.ceilIndex) {
    return MStatus::kSuccess;
  }

  mLastSampleInfo = sampleInfo;

  // access the camera values
  AbcG::ICurvesSchema::Sample sample;
  AbcG::ICurvesSchema::Sample sample2;
  mSchema.get(sample, sampleInfo.floorIndex);
  if (sampleInfo.alpha != 0.0) {
    mSchema.get(sample2, sampleInfo.ceilIndex);
  }

  Abc::P3fArraySamplePtr samplePos = sample.getPositions();
  Abc::P3fArraySamplePtr samplePos2;
  if (sampleInfo.alpha != 0.0) {
    samplePos2 = sample2.getPositions();
  }

  // iteration should not be necessary. the iteration is only
  // required if the same mesh is attached to the same deformer
  // several times
  float blend = (float)sampleInfo.alpha;
  float iblend = 1.0f - blend;
  unsigned int index = 0;
  for (iter.reset(); !iter.isDone(); iter.next()) {
    index = iter.index();
    // MFloatPoint pt = iter.position();
    MPoint pt = iter.position();
    MPoint abcPos = pt;
    float weight = weightValue(dataBlock, geomIndex, index) * env;
    if (weight == 0.0f) {
      continue;
    }
    float iweight = 1.0f - weight;
    if (index >= samplePos->size()) {
      continue;
    }
    bool done = false;
    if (sampleInfo.alpha != 0.0) {
      if (samplePos2->size() == samplePos->size()) {
        abcPos.x = iweight * pt.x +
                   weight * (samplePos->get()[index].x * iblend +
                             samplePos2->get()[index].x * blend);
        abcPos.y = iweight * pt.y +
                   weight * (samplePos->get()[index].y * iblend +
                             samplePos2->get()[index].y * blend);
        abcPos.z = iweight * pt.z +
                   weight * (samplePos->get()[index].z * iblend +
                             samplePos2->get()[index].z * blend);
        done = true;
      }
    }
    if (!done) {
      abcPos.x = iweight * pt.x + weight * samplePos->get()[index].x;
      abcPos.y = iweight * pt.y + weight * samplePos->get()[index].y;
      abcPos.z = iweight * pt.z + weight * samplePos->get()[index].z;
    }
    iter.setPosition(abcPos);
  }
  return MStatus::kSuccess;
}

// Cache the plug arrays for use in setDependentsDirty
bool AlembicCurvesDeformNode::setInternalValueInContext(const MPlug & plug,
    const MDataHandle & dataHandle,
    MDGContext &)
{
  if (plug == mGeomParamsList) {
    MString geomParamsStr = dataHandle.asString();
    getPlugArrayFromAttrList(geomParamsStr, thisMObject(), mGeomParamPlugs);
  }
  else if (plug == mUserAttrsList) {
    MString userAttrsStr = dataHandle.asString();
    getPlugArrayFromAttrList(userAttrsStr, thisMObject(), mUserAttrPlugs);
  }

  return false;
}

MStatus AlembicCurvesDeformNode::setDependentsDirty(const MPlug &plugBeingDirtied,
    MPlugArray &affectedPlugs)
{
  if (plugBeingDirtied == mFileNameAttr || plugBeingDirtied == mIdentifierAttr
      || plugBeingDirtied == mTimeAttr) {

    for (unsigned int i = 0; i < mGeomParamPlugs.length(); i++) {
      affectedPlugs.append(mGeomParamPlugs[i]);
    }

    for (unsigned int i = 0; i < mUserAttrPlugs.length(); i++) {
      affectedPlugs.append(mUserAttrPlugs[i]);
    }
  }

  return MStatus::kSuccess;
}

AlembicCurvesLocatorNode::AlembicCurvesLocatorNode()
{
  mNbCurves = 0;
  mNbVertices = 0;
  mBoundingBox.clear();
  mSent = 0;
}

void AlembicCurvesLocatorNode::PreDestruction()
{
  mSchema.reset();
  delRefArchive(mFileName);
  mFileName.clear();
}

AlembicCurvesLocatorNode::~AlembicCurvesLocatorNode() { PreDestruction(); }
MObject AlembicCurvesLocatorNode::mTimeAttr;
MObject AlembicCurvesLocatorNode::mFileNameAttr;
MObject AlembicCurvesLocatorNode::mIdentifierAttr;
MObject AlembicCurvesLocatorNode::mSentinelAttr;

MStatus AlembicCurvesLocatorNode::initialize()
{
  MStatus status;

  MFnUnitAttribute uAttr;
  MFnTypedAttribute tAttr;
  MFnNumericAttribute nAttr;
  MFnGenericAttribute gAttr;
  MFnStringData emptyStringData;
  MObject emptyStringObject = emptyStringData.create("");

  // input time
  mTimeAttr = uAttr.create("inTime", "tm", MFnUnitAttribute::kTime, 0.0);
  status = uAttr.setStorable(true);
  status = uAttr.setKeyable(true);
  status = addAttribute(mTimeAttr);

  // input file name
  mFileNameAttr =
      tAttr.create("fileName", "fn", MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setUsedAsFilename(true);
  status = tAttr.setKeyable(false);
  status = addAttribute(mFileNameAttr);

  // input identifier
  mIdentifierAttr =
      tAttr.create("identifier", "if", MFnData::kString, emptyStringObject);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = addAttribute(mIdentifierAttr);

  // sentinel attr
  mSentinelAttr = nAttr.create("sentinel", "sent", MFnNumericData::kInt, 0);
  nAttr.setHidden(true);
  status = addAttribute(mSentinelAttr);

  // create a mapping
  status = attributeAffects(mTimeAttr, mSentinelAttr);
  status = attributeAffects(mFileNameAttr, mSentinelAttr);
  status = attributeAffects(mIdentifierAttr, mSentinelAttr);

  return status;
}

MStatus AlembicCurvesLocatorNode::compute(const MPlug &plug,
                                          MDataBlock &dataBlock)
{
  ESS_PROFILE_SCOPE("AlembicCurvesLocatorNode::compute");
  MStatus status;

  // update the frame number to be imported
  double inputTime =
      dataBlock.inputValue(mTimeAttr).asTime().as(MTime::kSeconds);
  MString &fileName = dataBlock.inputValue(mFileNameAttr).asString();
  MString &identifier = dataBlock.inputValue(mIdentifierAttr).asString();

  AbcG::ICurves obj;

  // check if we have the file
  if (fileName != mFileName || identifier != mIdentifier) {
    mSchema.reset();
    if (fileName != mFileName) {
      delRefArchive(mFileName);
      mFileName = fileName;
      addRefArchive(mFileName);
    }
    mIdentifier = identifier;

    // get the object from the archive
    Abc::IObject iObj = getObjectFromArchive(mFileName, identifier);
    if (!iObj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' not found in archive '" + mFileName + "'.");
      return MStatus::kFailure;
    }
    obj = AbcG::ICurves(iObj, Abc::kWrapExisting);
    if (!obj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' in archive '" + mFileName +
                              "' is not a Curves.");
      return MStatus::kFailure;
    }
    mSchema = obj.getSchema();
  }

  if (!mSchema.valid()) {
    return MStatus::kFailure;
  }

  // get the sample
  SampleInfo sampleInfo = getSampleInfo(inputTime, mSchema.getTimeSampling(),
                                        mSchema.getNumSamples());

  // check if we have to do this at all
  if (mNbCurves == 0 || mLastSampleInfo.floorIndex != sampleInfo.floorIndex ||
      mLastSampleInfo.ceilIndex != sampleInfo.ceilIndex) {
    AbcG::ICurvesSchema::Sample sample;
    AbcG::ICurvesSchema::Sample sample2;
    mSchema.get(sample, sampleInfo.floorIndex);
    if (sampleInfo.alpha != 0.0) {
      mSchema.get(sample2, sampleInfo.ceilIndex);
    }

    // update the indices
    Abc::P3fArraySamplePtr samplePos = sample.getPositions();
    if (mNbCurves != sample.getNumCurves() ||
        mNbVertices != samplePos->size()) {
      mNbCurves = (unsigned int)sample.getNumCurves();
      mNbVertices = (unsigned int)samplePos->size();

      Abc::Int32ArraySamplePtr nbVertices = sample.getCurvesNumVertices();
      mIndices.clear();
      unsigned int offset = 0;
      for (unsigned int i = 0; i < mNbCurves; i++) {
        unsigned int verticesPerCurve = nbVertices->get()[i];
        for (unsigned j = 0; j < verticesPerCurve - 1; j++) {
          mIndices.push_back(offset);
          offset++;
          mIndices.push_back(offset);
        }
        offset++;
      }
    }

    if (mPositions.size() != samplePos->size()) {
      mPositions.resize(samplePos->size());
    }

    // check if we need to interpolate
    bool done = false;
    mBoundingBox.clear();
    if (sampleInfo.alpha != 0.0) {
      Abc::P3fArraySamplePtr samplePos2 = sample2.getPositions();
      if (samplePos->size() == samplePos2->size()) {
        float alpha = float(sampleInfo.alpha);
        float ialpha = 1.0f - alpha;
        for (unsigned int i = 0; i < samplePos->size(); i++) {
          mPositions[i].x =
              ialpha * samplePos->get()[i].x + alpha * samplePos2->get()[i].x;
          mPositions[i].y =
              ialpha * samplePos->get()[i].y + alpha * samplePos2->get()[i].y;
          mPositions[i].z =
              ialpha * samplePos->get()[i].z + alpha * samplePos2->get()[i].z;
          mBoundingBox.expand(
              MPoint(mPositions[i].x, mPositions[i].y, mPositions[i].z));
        }
        done = true;
      }
    }

    if (!done) {
      for (unsigned int i = 0; i < samplePos->size(); i++) {
        mPositions[i].x = samplePos->get()[i].x;
        mPositions[i].y = samplePos->get()[i].y;
        mPositions[i].z = samplePos->get()[i].z;
        mBoundingBox.expand(
            MPoint(mPositions[i].x, mPositions[i].y, mPositions[i].z));
      }
    }

    // get the colors
    // mColors.clear();

    Abc::IC4fArrayProperty propColor;
    if (getArbGeomParamPropertyAlembic(obj, "color", propColor)) {
      mColors.clear();
      SampleInfo colorSampleInfo = getSampleInfo(
          inputTime, propColor.getTimeSampling(), propColor.getNumSamples());
      Abc::C4fArraySamplePtr sampleColor =
          propColor.getValue(colorSampleInfo.floorIndex);
      mColors.resize(mPositions.size());
      if (sampleColor->size() == 1) {
        for (unsigned int i = 0; i < (unsigned int)mColors.size(); i++) {
          mColors[i].r = sampleColor->get()[0].r;
          mColors[i].g = sampleColor->get()[0].g;
          mColors[i].b = sampleColor->get()[0].b;
          mColors[i].a = sampleColor->get()[0].a;
        }
      }
      else if (sampleColor->size() == mPositions.size()) {
        for (unsigned int i = 0; i < sampleColor->size(); i++) {
          mColors[i].r = sampleColor->get()[i].r;
          mColors[i].g = sampleColor->get()[i].g;
          mColors[i].b = sampleColor->get()[i].b;
          mColors[i].a = sampleColor->get()[i].a;
        }
      }
      else if (sampleColor->size() == mNbCurves) {
        Abc::Int32ArraySamplePtr nbVertices = sample.getCurvesNumVertices();
        unsigned int offset = 0;
        for (unsigned int i = 0; i < nbVertices->size(); i++) {
          for (unsigned j = 0; j < (unsigned int)nbVertices->get()[i]; j++) {
            mColors[offset].r = sampleColor->get()[i].r;
            mColors[offset].g = sampleColor->get()[i].g;
            mColors[offset].b = sampleColor->get()[i].b;
            mColors[offset].a = sampleColor->get()[i].a;
            offset++;
          }
        }
      }
    }
  }

  mLastSampleInfo = sampleInfo;

  MDataHandle outSent = dataBlock.outputValue(mSentinelAttr);
  // increment, this tells the draw routine that the display list needs to be
  // regenerated
  outSent.set((mSent + 1 % 10));
  dataBlock.setClean(mSentinelAttr);

  return MStatus::kSuccess;
}

bool AlembicCurvesLocatorNode::updated()
{
  MPlug SentinelPlug(thisMObject(), AlembicCurvesLocatorNode::mSentinelAttr);
  int sent = 0;
  SentinelPlug.getValue(sent);
  if (sent != mSent) {
    mSent = sent;
    return true;
  }
  return false;
}

MBoundingBox AlembicCurvesLocatorNode::boundingBox() const
{
  MPlug SentinelPlug(thisMObject(), AlembicCurvesLocatorNode::mSentinelAttr);
  int sent = 0;
  SentinelPlug.getValue(sent);
  return mBoundingBox;
}

void AlembicCurvesLocatorNode::draw(M3dView &view, const MDagPath &path,
                                    M3dView::DisplayStyle style,
                                    M3dView::DisplayStatus status)
{
  // I don't know how to use VBO's here, which of course would speed things
  // up significantly.

  updated();
  if (mIndices.size() == 0 || mPositions.size() == 0) {
    return;
  }

  // prepare draw
  view.beginGL();
  glPushAttrib(GL_CURRENT_BIT);

  // draw
  glBegin(GL_LINES);
  if (mColors.size() == mPositions.size()) {
    for (size_t i = 0; i < mIndices.size(); i++) {
      Abc::C4f &color = mColors[mIndices[i]];
      glColor4f(color.r, color.g, color.b, color.a);
      Abc::V3f &pos = mPositions[mIndices[i]];
      glVertex3f(pos.x, pos.y, pos.z);
    }
  }
  else {
    glColor4f(0.0, 0.0, 0.0, 1.0);
    for (size_t i = 0; i < mIndices.size(); i++) {
      Abc::V3f &pos = mPositions[mIndices[i]];
      glVertex3f(pos.x, pos.y, pos.z);
    }
  }
  glEnd();

  // end draw
  glPopAttrib();
  view.endGL();
}
