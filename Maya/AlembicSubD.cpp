#include "stdafx.h"

#include "AlembicSubD.h"
#include "AttributesReading.h"
#include "MetaData.h"

AlembicSubD::AlembicSubD(SceneNodePtr eNode, AlembicWriteJob* in_Job,
                         Abc::OObject oParent)
    : AlembicObject(eNode, in_Job, oParent)
{
  const bool animTS = (GetJob()->GetAnimatedTs() > 0);
  mObject = AbcG::OSubD(GetMyParent(), eNode->name, animTS);
  mSchema = mObject.getSchema();
}

AlembicSubD::~AlembicSubD()
{
  mObject.reset();
  mSchema.reset();
}

MStatus AlembicSubD::Save(double time, unsigned int timeIndex,
    bool isFirstFrame)
{
  ESS_PROFILE_SCOPE("AlembicSubD::Save");
  // access the geometry
  MFnSubd node(GetRef());

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
  bool globalCache = GetJob()->GetOption(L"exportInGlobalSpace").asInt() > 0;
  Abc::M44f globalXfo;
  if (globalCache) {
    globalXfo = GetGlobalMatrix(GetRef());
  }

  // access the points
  mPosVec.resize(node.vertexCount());
  MPointArray positions;
  node.vertexBaseMeshGet(positions);
  for (unsigned int i = 0; i < node.vertexCount(); i++) {
    const MPoint& out = positions[i];
    Imath::V3f& in = mPosVec[i];
    in.x = (float)out.x;
    in.y = (float)out.y;
    in.z = (float)out.z;
    if (globalCache) {
      globalXfo.multVecMatrix(in, in);
    }
    bbox.extendBy(in);
  }

  // store the positions to the samples
  mSample.setPositions(Abc::P3fArraySample(mPosVec));
  mSample.setSelfBounds(bbox);

  // check if we are doing pure pointcache
  if (GetJob()->GetOption(L"exportPurePointCache").asInt() > 0) {
    if (mNumSamples == 0) {
      mSample.setFaceCounts(Abc::Int32ArraySample(mFaceCountVec));
      mSample.setFaceIndices(Abc::Int32ArraySample(mFaceIndicesVec));
    }
    mSchema.set(mSample);
    mNumSamples++;
    return MStatus::kSuccess;
  }

  const bool dynamicTopology =
      GetJob()->GetOption(L"exportDynamicTopology").asInt() > 0;
  if (mNumSamples == 0 || dynamicTopology) {
    unsigned int polyCount = node.polygonCount();

    mFaceCountVec.reserve(polyCount);
    mFaceIndicesVec.reserve(polyCount * 4);
    mSampleLookup.reserve(polyCount * 4);
    unsigned int offset = 0;
    for (unsigned int i = 0; i < polyCount; i++) {
      MUint64Array indices;
      unsigned int count =
          node.polygonVertexCount(MFnSubdNames::baseFaceIdFromIndex(i));
      node.polygonVertices(MFnSubdNames::baseFaceIdFromIndex(i), indices);
      mFaceCountVec.push_back(count);
      for (unsigned int j = 0; j < (unsigned int)count; j++) {
        mSampleLookup.push_back(offset + count - j - 1);
        mFaceIndicesVec.push_back(
            node.vertexBaseIndexFromVertexId(indices[count - j - 1]));
      }
      offset += count;
    }

    Abc::Int32ArraySample faceCountSample(mFaceCountVec);
    Abc::Int32ArraySample faceIndicesSample(mFaceIndicesVec);
    mSample.setFaceCounts(faceCountSample);
    mSample.setFaceIndices(faceIndicesSample);

    // check if we need to export uvs
    if (GetJob()->GetOption(L"exportUVs").asInt() > 0) {
      if (node.polygonHasVertexUVs(MFnSubdNames::baseFaceIdFromIndex(0))) {
        unsigned int uvCount = (unsigned int)mSampleLookup.size();
        mUvVec.resize(uvCount);
        unsigned int offset = 0;
        for (unsigned int i = 0; i < polyCount; i++) {
          MDoubleArray uValues;
          MDoubleArray vValues;
          unsigned int count =
              node.polygonVertexCount(MFnSubdNames::baseFaceIdFromIndex(i));
          node.polygonGetVertexUVs(MFnSubdNames::baseFaceIdFromIndex(i),
                                   uValues, vValues);
          for (unsigned j = 0; j < count; ++j, ++offset) {
            const unsigned int sLookUp = mSampleLookup[offset];
            mUvVec[sLookUp].x = (float)uValues[j];
            mUvVec[sLookUp].y = (float)vValues[j];
          }
        }

        // now let's sort the uvs
        unsigned int uvIndexCount = 0;
        if (GetJob()->GetOption(L"indexedUVs").asInt() > 0) {
          std::map<SortableV2f, size_t> uvMap;
          std::map<SortableV2f, size_t>::const_iterator it;
          unsigned int sortedUVCount = 0;
          std::vector<Abc::V2f> sortedUVVec;
          mUvIndexVec.resize(mUvVec.size());
          sortedUVVec.resize(mUvVec.size());

          // loop over all uvs
          for (size_t i = 0; i < mUvVec.size(); i++) {
            it = uvMap.find(mUvVec[i]);
            if (it != uvMap.end()) {
              mUvIndexVec[uvIndexCount++] = (Abc::uint32_t)it->second;
            }
            else {
              mUvIndexVec[uvIndexCount++] = (Abc::uint32_t)sortedUVCount;
              uvMap.insert(std::pair<Abc::V2f, size_t>(
                  mUvVec[i], (Abc::uint32_t)sortedUVCount));
              sortedUVVec[sortedUVCount++] = mUvVec[i];
            }
          }

          // use indexed uvs if they use less space
          if (sortedUVCount * sizeof(Abc::V2f) +
                  uvIndexCount * sizeof(Abc::uint32_t) <
              sizeof(Abc::V2f) * mUvVec.size()) {
            mUvVec = sortedUVVec;
            uvCount = sortedUVCount;
          }
          else {
            uvIndexCount = 0;
            mUvIndexVec.clear();
          }
          sortedUVCount = 0;
          sortedUVVec.clear();
        }

        AbcG::OV2fGeomParam::Sample uvSample(Abc::V2fArraySample(mUvVec),
                                             AbcG::kFacevaryingScope);
        if (mUvIndexVec.size() > 0 && uvIndexCount > 0) {
          uvSample.setIndices(Abc::UInt32ArraySample(mUvIndexVec));
        }
        mSample.setUVs(uvSample);
      }
    }

    // loop for facesets
    std::size_t attrCount = node.attributeCount();
    for (unsigned int i = 0; i < attrCount; ++i) {
      MObject attr = node.attribute(i);
      MFnAttribute mfnAttr(attr);
      MPlug plug = node.findPlug(attr, true);

      // if it is not readable, then bail without any more checking
      if (!mfnAttr.isReadable() || plug.isNull()) {
        continue;
      }

      MString propName = plug.partialName(0, 0, 0, 0, 0, 1);
      std::string propStr = propName.asChar();
      if (propStr.substr(0, 8) == "FACESET_") {
        MStatus status;
        MFnIntArrayData arr(plug.asMObject(), &status);
        if (status != MS::kSuccess) {
          continue;
        }

        std::string faceSetName = propStr.substr(8);
        std::size_t numData = arr.length();
        std::vector<AbcU::int32_t> faceVals(numData);
        for (unsigned int j = 0; j < numData; ++j) {
          faceVals[j] = arr[j];
        }

        AbcG::OFaceSet faceSet = mSchema.createFaceSet(faceSetName);
        AbcG::OFaceSetSchema::Sample faceSetSample;
        faceSetSample.setFaces(Abc::Int32ArraySample(faceVals));
        faceSet.getSchema().set(faceSetSample);
      }
    }
  }

  // store the display resolution
  MObject attr = node.attribute("dispResolution");
  MFnAttribute mfnAttr(attr);
  MPlug plug = node.findPlug(attr, true);
  mSample.setFaceVaryingInterpolateBoundary(plug.asInt());

  // save the sample
  mSchema.set(mSample);
  mNumSamples++;

  return MStatus::kSuccess;
}

void AlembicSubDNode::PreDestruction()
{
  mSchema.reset();
  delRefArchive(mFileName);
  mFileName.clear();
}

AlembicSubDNode::~AlembicSubDNode() { PreDestruction(); }
MObject AlembicSubDNode::mTimeAttr;
MObject AlembicSubDNode::mFileNameAttr;
MObject AlembicSubDNode::mIdentifierAttr;
MObject AlembicSubDNode::mUvsAttr;
MObject AlembicSubDNode::mOutGeometryAttr;
MObject AlembicSubDNode::mOutDispResolutionAttr;

MObject AlembicSubDNode::mGeomParamsList;
MObject AlembicSubDNode::mUserAttrsList;

MStatus AlembicSubDNode::initialize()
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

  // input uvs
  mUvsAttr = nAttr.create("uvs", "uv", MFnNumericData::kBoolean, 1.0);
  status = tAttr.setStorable(true);
  status = tAttr.setKeyable(false);
  status = addAttribute(mUvsAttr);

  // output subd
  mOutGeometryAttr = tAttr.create("outSubdiv", "os", MFnData::kSubdSurface);
  status = tAttr.setStorable(false);
  status = tAttr.setWritable(false);
  status = tAttr.setKeyable(false);
  status = tAttr.setHidden(false);
  status = addAttribute(mOutGeometryAttr);

  // output disp resolution
  mOutDispResolutionAttr =
      nAttr.create("dispResolution", "dr", MFnNumericData::kInt, 1.0);
  status = nAttr.setStorable(false);
  status = nAttr.setWritable(false);
  status = nAttr.setKeyable(false);
  status = nAttr.setHidden(false);
  status = addAttribute(mOutDispResolutionAttr);

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
  status = attributeAffects(mUvsAttr, mOutGeometryAttr);
  status = attributeAffects(mTimeAttr, mOutDispResolutionAttr);
  status = attributeAffects(mFileNameAttr, mOutDispResolutionAttr);
  status = attributeAffects(mIdentifierAttr, mOutDispResolutionAttr);
  return status;
}

MStatus AlembicSubDNode::compute(const MPlug& plug, MDataBlock& dataBlock)
{
  ESS_PROFILE_SCOPE("AlembicSubDNode::compute");
  MStatus status;

  // update the frame number to be imported
  double inputTime =
      dataBlock.inputValue(mTimeAttr).asTime().as(MTime::kSeconds);
  MString& fileName = dataBlock.inputValue(mFileNameAttr).asString();
  MString& identifier = dataBlock.inputValue(mIdentifierAttr).asString();
  bool importUvs = dataBlock.inputValue(mUvsAttr).asBool();

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
    AbcG::ISubD obj(iObj, Abc::kWrapExisting);
    if (!obj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' in archive '" + mFileName +
                              "' is not a SubD.");
      return MStatus::kFailure;
    }
    mSchema = obj.getSchema();
    mSubDData = MObject::kNullObj;
  }

  if (!mSchema.valid()) {
    return MStatus::kFailure;
  }

  {
    ESS_PROFILE_SCOPE("AlembicSubDNode::compute readProps");
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
  if (!mSubDData.isNull() &&
      mLastSampleInfo.floorIndex == sampleInfo.floorIndex &&
      mLastSampleInfo.ceilIndex == sampleInfo.ceilIndex) {
    return MStatus::kSuccess;
  }

  mLastSampleInfo = sampleInfo;

  // access the camera values
  AbcG::ISubDSchema::Sample sample;
  AbcG::ISubDSchema::Sample sample2;
  mSchema.get(sample, sampleInfo.floorIndex);
  if (sampleInfo.alpha != 0.0) {
    mSchema.get(sample2, sampleInfo.ceilIndex);
  }

  // create the output subd
  if (mSubDData.isNull()) {
    MFnSubdData subdDataFn;
    mSubDData = subdDataFn.create();
  }

  Abc::P3fArraySamplePtr samplePos = sample.getPositions();
  Abc::Int32ArraySamplePtr sampleCounts = sample.getFaceCounts();
  Abc::Int32ArraySamplePtr sampleIndices = sample.getFaceIndices();

  // ensure that we are not running on a purepoint cache mesh
  if (sampleCounts->get()[0] == 0) {
    return MStatus::kFailure;
  }

  MPointArray points;
  if (samplePos->size() > 0) {
    points.setLength((unsigned int)samplePos->size());
    bool done = false;
    if (sampleInfo.alpha != 0.0) {
      Abc::P3fArraySamplePtr samplePos2 = sample2.getPositions();
      if (points.length() == (unsigned int)samplePos2->size()) {
        float blend = (float)sampleInfo.alpha;
        float iblend = 1.0f - blend;
        for (unsigned int i = 0; i < points.length(); i++) {
          points[i].x =
              samplePos->get()[i].x * iblend + samplePos2->get()[i].x * blend;
          points[i].y =
              samplePos->get()[i].y * iblend + samplePos2->get()[i].y * blend;
          points[i].z =
              samplePos->get()[i].z * iblend + samplePos2->get()[i].z * blend;
        }
        done = true;
      }
    }

    if (!done) {
      for (unsigned int i = 0; i < points.length(); i++) {
        points[i].x = samplePos->get()[i].x;
        points[i].y = samplePos->get()[i].y;
        points[i].z = samplePos->get()[i].z;
      }
    }
  }

  MIntArray counts;
  MIntArray indices;
  counts.setLength((unsigned int)sampleCounts->size());
  indices.setLength((unsigned int)sampleIndices->size());
  mSampleLookup.resize(indices.length());

  unsigned int offset = 0;
  for (unsigned int i = 0; i < counts.length(); i++) {
    counts[i] = sampleCounts->get()[i];
    for (int j = 0; j < counts[i]; j++) {
      MString count, index;
      count.set((double)counts[i]);
      index.set((double)sampleIndices->get()[offset + j]);
      mSampleLookup[offset + counts[i] - j - 1] = offset + j;
      indices[offset + j] = sampleIndices->get()[offset + counts[i] - j - 1];
    }
    offset += counts[i];
  }

  // create a subd either with or without uvs
  mSubD.createBaseMesh(false, points.length(), counts.length(), points, counts,
                       indices, mSubDData);
  // mSubD.updateSubdSurface();

  // check if we need to import uvs
  if (importUvs) {
    AbcG::IV2fGeomParam uvsParam = mSchema.getUVsParam();
    if (uvsParam.valid()) {
      if (uvsParam.getNumSamples() > 0) {
        sampleInfo = getSampleInfo(inputTime, uvsParam.getTimeSampling(),
                                   uvsParam.getNumSamples());

        Abc::V2fArraySamplePtr sampleUvs =
            uvsParam.getExpandedValue(sampleInfo.floorIndex).getVals();
        if (sampleUvs->size() == (size_t)indices.length()) {
          unsigned int offset = 0;
          MUint64Array indices;
          for (unsigned int i = 0; i < mSubD.polygonCount(); i++) {
            unsigned int count =
                mSubD.polygonVertexCount(MFnSubdNames::baseFaceIdFromIndex(i));
            MDoubleArray uValues, vValues;
            uValues.setLength(count);
            vValues.setLength(count);
            for (unsigned int j = 0; j < count; j++) {
              uValues[count - j - 1] = sampleUvs->get()[offset].x;
              vValues[count - j - 1] = sampleUvs->get()[offset].y;
              offset++;
            }
            mSubD.polygonSetVertexUVs(MFnSubdNames::baseFaceIdFromIndex(i),
                                      uValues, vValues);
            mSubD.polygonSetUseUVs(MFnSubdNames::baseFaceIdFromIndex(i), true);
          }
        }
      }
    }
  }

  // output all channels
  dataBlock.outputValue(mOutGeometryAttr).set(mSubDData);
  dataBlock.outputValue(mOutDispResolutionAttr)
      .set((int)sample.getFaceVaryingInterpolateBoundary());

  // clean output channels
  dataBlock.outputValue(mOutGeometryAttr).setClean();
  dataBlock.outputValue(mOutDispResolutionAttr).setClean();

  return MStatus::kSuccess;
}

// Cache the plug arrays for use in setDependentsDirty
bool AlembicSubDNode::setInternalValueInContext(const MPlug & plug,
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

MStatus AlembicSubDNode::setDependentsDirty(const MPlug &plugBeingDirtied,
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

void AlembicSubDDeformNode::PreDestruction()
{
  mSchema.reset();
  delRefArchive(mFileName);
  mFileName.clear();
}

AlembicSubDDeformNode::~AlembicSubDDeformNode() { PreDestruction(); }
MObject AlembicSubDDeformNode::mTimeAttr;
MObject AlembicSubDDeformNode::mFileNameAttr;
MObject AlembicSubDDeformNode::mIdentifierAttr;

MObject AlembicSubDDeformNode::mGeomParamsList;
MObject AlembicSubDDeformNode::mUserAttrsList;

MStatus AlembicSubDDeformNode::initialize()
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

MStatus AlembicSubDDeformNode::compute(const MPlug& plug, MDataBlock& dataBlock)
{
  ESS_PROFILE_SCOPE("AlembicSubDDeformNode::compute");
  if (plug.attribute() == outputGeom && mVertexLookup.size() == 0) {
    unsigned int index = plug.logicalIndex();
    MObject thisNode = this->thisMObject();
    MPlug inPlug(thisNode, input);
    inPlug.selectAncestorLogicalIndex(index, input);
    MDataHandle hInput = dataBlock.inputValue(inPlug);

    MDataHandle hGeom = hInput.child(inputGeom);
    MFnSubd subDiv(hGeom.asSubdSurface());

    MFnSubdNames names;

    unsigned int count = subDiv.polygonCount();
    std::vector<bool> done(subDiv.vertexCount());
    for (size_t i = 0; i < done.size(); i++) {
      done[i] = false;
    }

    MUint64Array vertices;
    for (index = 0; index < count; index++) {
      MUint64 id = names.baseFaceIdFromIndex(index);
      subDiv.polygonVertices(id, vertices);
      for (unsigned int i = 0; i < vertices.length(); i++) {
        unsigned int j = subDiv.vertexBaseIndexFromVertexId(vertices[i]);
        if (done[j]) {
          continue;
        }
        done[j] = true;
        mVertexLookup.push_back(j);
      }
    }
  }
  return AlembicObjectDeformNode::compute(plug, dataBlock);
}

MStatus AlembicSubDDeformNode::deform(MDataBlock& dataBlock, MItGeometry& iter,
                                      const MMatrix& localToWorld,
                                      unsigned int geomIndex)
{
  ESS_PROFILE_SCOPE("AlembicSubDDeformNode::deform");
  // get the envelope data
  float env = dataBlock.inputValue(envelope).asFloat();
  if (env == 0.0f) {  // deformer turned off
    return MStatus::kSuccess;
  }

  // update the frame number to be imported
  double inputTime =
      dataBlock.inputValue(mTimeAttr).asTime().as(MTime::kSeconds);
  MString& fileName = dataBlock.inputValue(mFileNameAttr).asString();
  MString& identifier = dataBlock.inputValue(mIdentifierAttr).asString();

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
    AbcG::ISubD obj(iObj, Abc::kWrapExisting);
    if (!obj.valid()) {
      MGlobal::displayWarning("[ExocortexAlembic] Identifier '" + identifier +
                              "' in archive '" + mFileName +
                              "' is not a SubD.");
      return MStatus::kFailure;
    }
    mSchema = obj.getSchema();
  }

  if (!mSchema.valid()) {
    return MStatus::kFailure;
  }

  {
    ESS_PROFILE_SCOPE("AlembicSubDDeformNode::deform readProps");
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
  AbcG::ISubDSchema::Sample sample;
  AbcG::ISubDSchema::Sample sample2;
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
    if (index >= mVertexLookup.size()) {
      continue;
    }
    float weight = weightValue(dataBlock, geomIndex, index) * env;
    if (weight == 0.0f) {
      continue;
    }
    float iweight = 1.0f - weight;
    if (index >= samplePos->size()) {
      continue;
    }
    bool done = false;
    index = mVertexLookup[index];
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
bool AlembicSubDDeformNode::setInternalValueInContext(const MPlug & plug,
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

MStatus AlembicSubDDeformNode::setDependentsDirty(const MPlug &plugBeingDirtied,
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
