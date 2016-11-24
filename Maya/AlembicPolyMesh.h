#ifndef _ALEMBIC_POLYMESH_H_
#define _ALEMBIC_POLYMESH_H_

#include <maya/MFnMesh.h>
#include "AlembicObject.h"
#include "AttributesWriter.h"

class AlembicPolyMesh : public AlembicObject {
 private:
  AbcG::OPolyMesh mObject;
  AbcG::OPolyMeshSchema mSchema;
  int mPointCountLastFrame;
  std::vector<unsigned int> mSampleLookup;

  AttributesWriterPtr mAttrs;

  AbcG::OPolyMeshSchema::Sample mSample;
  std::vector<AbcG::OV2fGeomParam> mUvParams;

 public:
  AlembicPolyMesh(SceneNodePtr eNode, AlembicWriteJob* in_Job,
                  Abc::OObject oParent);
  ~AlembicPolyMesh();

  virtual Abc::OObject GetObject() { return mObject; }
  virtual Abc::OCompoundProperty GetCompound() { return mSchema; }
  virtual MStatus Save(double time, unsigned int timeIndex,
      bool isFirstFrame);
};

class AlembicPolyMeshNode : public AlembicObjectNode {
 public:
  AlembicPolyMeshNode() : mUvFromDifferentFile(false) {}
  virtual ~AlembicPolyMeshNode();

  // override virtual methods from MPxNode
  virtual void PreDestruction();
  virtual MStatus compute(const MPlug& plug, MDataBlock& dataBlock);
  static void* creator() { return (new AlembicPolyMeshNode()); }
  static MStatus initialize();

  bool setInternalValueInContext(const MPlug & plug,
      const MDataHandle & dataHandle,
      MDGContext & ctx);
  MStatus setDependentsDirty(const MPlug &plugBeingDirtied,
      MPlugArray &affectedPlugs);

 private:
  // input attributes
  static MObject mTimeAttr;
  static MObject mFileNameAttr;
  static MObject mIdentifierAttr;
  static MObject mUvFileNameAttr;
  static MObject mUvIdentifierAttr;
  MString mFileName;
  MString mIdentifier;
  MString mUvFileName;
  MString mUvIdentifier;
  MPlugArray mGeomParamPlugs;
  MPlugArray mUserAttrPlugs;
  Abc::IObject mObj;
  AbcG::IPolyMeshSchema mSchema;
  AbcG::IPolyMeshSchema mUvSchema;
  bool mDynamicTopology;
  bool mUvFromDifferentFile;
  static MObject mNormalsAttr;
  static MObject mUvsAttr;

  // output attributes
  static MObject mOutGeometryAttr;

  static MObject mGeomParamsList;
  static MObject mUserAttrsList;

  // members
  SampleInfo mLastSampleInfo;
  MObject mMeshData;
  MFnMesh mMesh;
  std::vector<unsigned int> mSampleLookup;
  MIntArray mNormalFaces;
  MIntArray mNormalVertices;
};

class AlembicPolyMeshDeformNode : public AlembicObjectDeformNode {
 private:
  typedef MRUCache<Alembic::AbcCoreAbstract::index_t, Abc::P3fArraySamplePtr>
      mruP3fArraySamplePtr;

 public:
  AlembicPolyMeshDeformNode(void) : cachePosition() {}
  virtual ~AlembicPolyMeshDeformNode();
  // override virtual methods from MPxDeformerNode
  virtual void PreDestruction();
  virtual MStatus deform(MDataBlock& dataBlock, MItGeometry& iter,
                         const MMatrix& localToWorld, unsigned int geomIndex);
  static void* creator() { return (new AlembicPolyMeshDeformNode()); }
  static MStatus initialize();

  bool setInternalValueInContext(const MPlug & plug,
      const MDataHandle & dataHandle,
      MDGContext & ctx);
  MStatus setDependentsDirty(const MPlug &plugBeingDirtied,
      MPlugArray &affectedPlugs);

 private:
  // input attributes
  static MObject mTimeAttr;
  static MObject mFileNameAttr;
  static MObject mIdentifierAttr;
  MString mFileName;
  MString mIdentifier;
  MPlugArray mGeomParamPlugs;
  MPlugArray mUserAttrPlugs;
  Abc::IObject mObj;
  AbcG::IPolyMeshSchema mSchema;
  bool mDynamicTopology;

  // output attributes
  static MObject mGeomParamsList;
  static MObject mUserAttrsList;

  // members
  SampleInfo mLastSampleInfo;
  mruP3fArraySamplePtr cachePosition;
};

class AlembicCreateFaceSetsCommand : public MPxCommand {
 public:
  AlembicCreateFaceSetsCommand() {}
  virtual ~AlembicCreateFaceSetsCommand() {}
  virtual bool isUndoable() const { return false; }
  MStatus doIt(const MArgList& args);

  static MSyntax createSyntax();
  static void* creator() { return new AlembicCreateFaceSetsCommand(); }
};

#endif
