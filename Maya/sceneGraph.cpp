#include "stdafx.h"

#include "AttributesReading.h"
#include "sceneGraph.h"

#include "CommonLog.h"

#include <maya/MDGModifier.h>
#include <maya/MFnDagNode.h>
#include <maya/MItDag.h>

static MString __PythonBool[] = {MString("False"), MString("True")};
static inline const MString &PythonBool(bool b)
{
  return __PythonBool[b ? 1 : 0];
}

static void __file_and_time_control_kill(const MString &var)
{
  MGlobal::executePythonCommand(
      var + " = None\n");  // deallocate this variable in Python!
}

MObject findMObjectByName(const MString &obj)
{
  MSelectionList selList;
  selList.add(obj);

  MObject result;
  selList.getDependNode(0, result);

  return result;
}

AlembicFileAndTimeControl::~AlembicFileAndTimeControl(void)
{
  __file_and_time_control_kill(var);
}

AlembicFileAndTimeControlPtr AlembicFileAndTimeControl::createControl(
    const IJobStringParser &jobParams)
{
  static unsigned int numberOfControlCreated = 0;
  static const MString format(
      "^1s = ExoAlembic._import.IJobInfo(r\"^2s\", ^3s, ^4s, ^5s, ^6s)\n");

  ESS_PROFILE_SCOPE("AlembicFileAndTimeControl::createControl");
  MString var("__alembic_file_and_time_control_tuple_");
  var += (++numberOfControlCreated);

  MString cmd;
  cmd.format(
      format, var, jobParams.filename.c_str(),
      PythonBool(jobParams.importNormals), PythonBool(jobParams.importUVs),
      PythonBool(jobParams.importFacesets), PythonBool(jobParams.useMultiFile));
  MStatus status = MGlobal::executePythonCommand(cmd);
  if (status != MS::kSuccess) {
    __file_and_time_control_kill(var);
    return AlembicFileAndTimeControlPtr();
  }
  return AlembicFileAndTimeControlPtr(new AlembicFileAndTimeControl(var));
}

bool SceneNodeMaya::replaceData(SceneNodeAlembicPtr fileNode,
                                const IJobStringParser &jobParams,
                                SceneNodeAlembicPtr &nextFileNode)
{
  ESS_PROFILE_SCOPE("SceneNodeMaya::replaceData");
  nextFileNode = fileNode;
  switch (nextFileNode->type) {
    case ETRANSFORM:
    case ITRANSFORM:
      return replaceSimilarData<Alembic::AbcGeom::IXform,
             Alembic::AbcGeom::IXformSchema>("Xform", fileNode);
    case CAMERA:
      return replaceSimilarData<Alembic::AbcGeom::ICamera,
             Alembic::AbcGeom::ICameraSchema>("Camera", fileNode);
    case POLYMESH:
    case SUBD:
      return replaceSimilarData<Alembic::AbcGeom::IPolyMesh,
             Alembic::AbcGeom::IPolyMeshSchema>("PolyMesh", fileNode);
    case CURVES:
    case HAIR:
      return replaceSimilarData<Alembic::AbcGeom::ICurves,
             Alembic::AbcGeom::ICurvesSchema>("Curves", fileNode);
    case PARTICLES:
      return replaceSimilarData<Alembic::AbcGeom::IPoints,
             Alembic::AbcGeom::IPointsSchema>("Points", fileNode);
    // case SURFACE:	// handle as default for now
    // break;
    // case LIGHT:	// handle as default for now
    // break;
    default:
      break;
  }
  return false;
}

bool SceneNodeMaya::connectProps(MFnDependencyNode &depNode,
    MFnDependencyNode &readerDepNode)
{
  MStatus status;
  MPlug geomParamsPlug = readerDepNode.findPlug("ExocortexAlembic_GeomParams",
      &status);
  MPlug userAttrsPlug = readerDepNode.findPlug("ExocortexAlembic_UserAttributes",
      &status);
  if (status != MStatus::kSuccess) {
    return false;
  }

  MString geomProp;
  status = geomParamsPlug.getValue(geomProp);
  MString userProp;
  status = userAttrsPlug.getValue(userProp);
  if (status != MStatus::kSuccess) {
    return false;
  }

  MStringArray geomProps;
  geomProp.split(';', geomProps);
  MStringArray userProps;
  userProp.split(';', userProps);

  MDGModifier mod;
  for (unsigned int i = 0; i < geomProps.length(); i++) {
    MStatus propStatus;
    MPlug readerPlug = readerDepNode.findPlug(geomProps[i], &propStatus);
    MPlug shapePlug = depNode.findPlug(geomProps[i], &propStatus);
    if (propStatus == MStatus::kSuccess) {
      mod.connect(readerPlug, shapePlug);
    }
  }

  for (unsigned int i = 0; i < userProps.length(); i++) {
    MStatus propStatus;
    MPlug readerPlug = readerDepNode.findPlug(userProps[i], &propStatus);
    MPlug shapePlug = depNode.findPlug(userProps[i], &propStatus);
    if (propStatus == MStatus::kSuccess) {
      mod.connect(readerPlug, shapePlug);
    }
  }

  status = mod.doIt();

  return (status == MStatus::kSuccess);
}

bool SceneNodeMaya::removeProps(const MString &dccReaderIdentifier)
{
  MStatus status;
  MObject readerNode = findMObjectByName(dccReaderIdentifier);
  MFnDependencyNode readerDepNode(readerNode, &status);

  MPlug geomParamsPlug = readerDepNode.findPlug("ExocortexAlembic_GeomParams",
      &status);
  MPlug userAttrsPlug = readerDepNode.findPlug("ExocortexAlembic_UserAttributes",
      &status);
  if (status != MStatus::kSuccess) {
    return false;
  }

  MString geomProp;
  status = geomParamsPlug.getValue(geomProp);
  MString userProp;
  status = userAttrsPlug.getValue(userProp);
  if (status != MStatus::kSuccess) {
    return false;
  }

  MStringArray geomProps;
  geomProp.split(';', geomProps);
  MStringArray userProps;
  userProp.split(';', userProps);

  MDGModifier mod;
  for (unsigned int i = 0; i < geomProps.length(); i++) {
    MStatus propStatus;
    MPlug readerPlug = readerDepNode.findPlug(geomProps[i], &propStatus);
    if (propStatus == MStatus::kSuccess) {
      mod.removeAttribute(readerNode, readerPlug);
    }
  }

  for (unsigned int i = 0; i < userProps.length(); i++) {
    MStatus propStatus;
    MPlug readerPlug = readerDepNode.findPlug(userProps[i], &propStatus);
    if (propStatus == MStatus::kSuccess) {
      mod.removeAttribute(readerNode, readerPlug);
    }
  }

  status = mod.doIt();

  geomParamsPlug.setValue("");
  userAttrsPlug.setValue("");

  return (status == MStatus::kSuccess);
}

bool SceneNodeMaya::executeAddChild(const MString &cmd,
                                    SceneNodeAppPtr &newAppNode)
{
  std::string cmdstr(cmd.asChar());
  MStringArray results;
  MGlobal::executePythonCommand(cmdstr.c_str(), results);
  if (results.length() == 0 || results[0].length() == 0) {
    return false;
  }
  else if (results.length() != 2 || results[1].length() == 0) {
#ifdef _DEBUG
    std::ostringstream resultsStream;
    resultsStream << results;
    MGlobal::displayError(resultsStream.str().c_str());
    MGlobal::displayError(cmd);
#endif
    return false;
  }

  newAppNode->dccIdentifier = results[0].asChar();
  newAppNode->name = newAppNode->dccIdentifier;
  newAppNode->dccReaderIdentifier = results[1].asChar();
  return true;
}

bool SceneNodeMaya::addSimilarChild(const char *functionName,
                                    SceneNodeAlembicPtr fileNode,
                                    SceneNodeAppPtr &newAppNode)
{
  static const MString format(
      "ExoAlembic._import.import^1s(r\"^2s\", r\"^3s\", ^4s, r\"^5s\", ^6s)");

  ESS_PROFILE_SCOPE("SceneNodeMaya::addSimilarChild");
  MString cmd;
  cmd.format(
      format, functionName, fileNode->name.c_str(),
      fileNode->dccIdentifier.c_str(), fileAndTime->variable(),
      dccIdentifier.c_str(),
      PythonBool(this->useMultiFile ? false : fileNode->pObjCache->isConstant));
  return executeAddChild(cmd, newAppNode);
}

bool SceneNodeMaya::addXformChild(SceneNodeAlembicPtr fileNode,
                                  SceneNodeAppPtr &newAppNode)
{
  static const MString format(
      "ExoAlembic._import.importXform(r\"^1s\", r\"^2s\", ^3s, ^4s, ^5s)");

  ESS_PROFILE_SCOPE("SceneNodeMaya::addXformChild");
  MString parent;
  switch (type) {
    case ETRANSFORM:
    case ITRANSFORM:
      parent.format("\"^1s\"", dccIdentifier.c_str());
      break;
    default:
      parent = "None";
      break;
  }

  MString cmd;
  cmd.format(format, fileNode->name.c_str(), fileNode->dccIdentifier.c_str(),
      fileAndTime->variable(), parent,
      PythonBool(fileNode->pObjCache->isConstant));
  if (!executeAddChild(cmd, newAppNode)) {
    return false;
  }

  return addAndConnectProps<Alembic::AbcGeom::IXform,
         Alembic::AbcGeom::IXformSchema>(
             newAppNode);
}

bool SceneNodeMaya::addPolyMeshChild(SceneNodeAlembicPtr fileNode,
                                     SceneNodeAppPtr &newAppNode)
{
  static const MString format(
      "ExoAlembic._import.importPolyMesh(r\"^1s\", r\"^2s\", ^3s, \"^4s\", "
      "^5s, ^6s)");

  ESS_PROFILE_SCOPE("SceneNodeMaya::addPolyMeshChild");
  MString cmd;
  cmd.format(format, fileNode->name.c_str(), fileNode->dccIdentifier.c_str(),
             fileAndTime->variable(), dccIdentifier.c_str(),
             PythonBool(fileNode->pObjCache->isConstant),
             PythonBool(fileNode->pObjCache->isMeshTopoDynamic));

  if (!executeAddChild(cmd, newAppNode)) {
    return false;
  }

  return addAndConnectProps<Alembic::AbcGeom::IPolyMesh,
         Alembic::AbcGeom::IPolyMeshSchema>(
             newAppNode);
}

bool SceneNodeMaya::addCurveChild(SceneNodeAlembicPtr fileNode,
                                  SceneNodeAppPtr &newAppNode)
{
  static const MString format(
      "ExoAlembic._import.importCurves(r\"^1s\", r\"^2s\", ^3s, r\"^4s\", ^5s, "
      "^6s)");

  MString strNb;
  {
    AbcG::ICurvesSchema::Sample sample;
    AbcG::ICurves obj(fileNode->getObject(), Abc::kWrapExisting);
    obj.getSchema().get(sample, 0);

    strNb += (unsigned int)sample.getCurvesNumVertices()->size();
  }

  MString cmd;
  cmd.format(format, fileNode->name.c_str(), fileNode->dccIdentifier.c_str(),
             fileAndTime->variable(), dccIdentifier.c_str(),
             PythonBool(fileNode->pObjCache->isConstant), strNb);
  if (!executeAddChild(cmd, newAppNode)) {
    return false;
  }

  return addAndConnectProps<Alembic::AbcGeom::ICurves,
         Alembic::AbcGeom::ICurvesSchema>(
             newAppNode);
}

bool SceneNodeMaya::addChild(SceneNodeAlembicPtr fileNode,
                             const IJobStringParser &jobParams,
                             SceneNodeAppPtr &newAppNode)
{
  ESS_PROFILE_SCOPE("SceneNodeMaya::addChild");
  this->useMultiFile = jobParams.useMultiFile;
  newAppNode.reset(new SceneNodeMaya(fileAndTime));
  switch (newAppNode->type = fileNode->type) {
    case ETRANSFORM:
    case ITRANSFORM:
      return addXformChild(fileNode, newAppNode);
    case CAMERA:
      if (!addSimilarChild("Camera", fileNode, newAppNode)) {
        return false;
      }
      return addAndConnectProps<Alembic::AbcGeom::ICamera,
             Alembic::AbcGeom::ICameraSchema>(
                 newAppNode);
    case POLYMESH:
    case SUBD:
      return addPolyMeshChild(fileNode, newAppNode);
    case CURVES:
    case HAIR:
      return addCurveChild(fileNode, newAppNode);
    case PARTICLES:
      if (!addSimilarChild("Points", fileNode, newAppNode)) {
        return false;
      }
      return addAndConnectProps<Alembic::AbcGeom::IPoints,
             Alembic::AbcGeom::IPointsSchema>(
                 newAppNode);
    // case SURFACE:	// handle as default for now
    // break;
    // case LIGHT:	// handle as default for now
    // break;
    default:
      break;
  }
  return false;
}

void SceneNodeMaya::print(void)
{
  ESS_LOG_WARNING("Maya Scene Node: " << dccIdentifier);
}

MObject getObjByName(const char *obj)
{
  MSelectionList selList;

  selList.add(obj);
  MObject result;
  selList.getDependNode(0, result);

  return result;
}

MString getDeformedName(const MString &name)
{
  int barPos = name.rindexW("|");
  int colonPos = name.rindexW(":");
  // Got a malformed path!
  if (barPos < 0) {
    return "";
  }
  // There's no namespace to strip
  if (colonPos < 0) {
    colonPos = barPos;
  }

  return name.substring(0, barPos) // Has the same parent
    + name.substring(colonPos + 1, name.length()) // Has no namespace
    + "Deformed"; // Has the "Deformed" suffix;
}

static bool visitChild(
    const MObject &mObj, SceneNodeAppPtr &parent,
    const AlembicFileAndTimeControlPtr &alembicFileAndTimeControl,
    const SearchReplace::ReplacePtr &replacer,
    bool allowDeformedByUs)
{
  MFnDagNode dagNode(mObj);
  if (dagNode.isIntermediateObject()) {  // skip intermediate object!
    if (!allowDeformedByUs) {
      return true;
    }
  }

  const std::string dccId = dagNode.fullPathName().asChar();
  if (dccId.length() == 0) {
    return true;
  }

  // check if it's a valid type of node first!
  SceneNodeMaya* sceneNode = new SceneNodeMaya(alembicFileAndTimeControl);
  SceneNodeAppPtr exoChild(sceneNode);
  switch (mObj.apiType()) {
    case MFn::kCamera:
      exoChild->type = SceneNode::CAMERA;
      break;
    case MFn::kMesh:
      exoChild->type = SceneNode::POLYMESH;
      break;
    case MFn::kSubdiv:
      exoChild->type = SceneNode::SUBD;
      break;
    case MFn::kNurbsCurve:
      exoChild->type = SceneNode::CURVES;
      break;
    case MFn::kParticle:
      exoChild->type = SceneNode::PARTICLES;
      break;
    case MFn::kPfxHair:
      exoChild->type = SceneNode::HAIR;
      break;
    case MFn::kTransform:
    default:
      exoChild->type = SceneNode::ETRANSFORM;
      break;
  }

  if (dagNode.isIntermediateObject()) {
    MString deformedName = getDeformedName(dccId.c_str());
    if (deformedName.length() == 0) {
      return false;
    }
    MObject deformedObj = getObjByName(deformedName.asChar());

    MStatus status;
    MFnDependencyNode depNode(deformedObj, &status);
    MPlug inMeshPlug = depNode.findPlug("inMesh", true, &status);
    if (status.error()) {
      return false;
    }

    MPlugArray connections;
    if (!inMeshPlug.connectedTo(connections, true, false)) {
      return false;
    }

    MFnDependencyNode connectedDepNode(connections[0].node());
    MPxNode* connectedNode = connectedDepNode.userNode(&status);
    if (status.error()) {
      return false;
    }

    if (connectedNode->typeName() != "ExocortexAlembicPolyMeshDeform") {
      return true;
    }

    sceneNode->connectTo = deformedName;
  }

  parent->children.push_back(exoChild);
  exoChild->parent = parent.get();

  exoChild->dccIdentifier = dccId;
  {
    const std::string rname =
        replacer->replace(dagNode.fullPathName().asChar());
    const size_t pos = rname.rfind("|");
    exoChild->name = (pos != std::string::npos) ? rname.substr(pos + 1) : rname;
  }
  for (unsigned int i = 0; i < dagNode.childCount(); ++i) {
    visitChild(dagNode.child(i), exoChild, alembicFileAndTimeControl, replacer,
        allowDeformedByUs);
  }
  return true;
}

SceneNodeAppPtr buildMayaSceneGraph(
    const MDagPath &dagPath, const SearchReplace::ReplacePtr &replacer,
    const AlembicFileAndTimeControlPtr alembicFileAndTimeControl,
    bool allowDeformedByUs)
{
  ESS_PROFILE_SCOPE("buildMayaSceneGraph");
  SceneNodeAppPtr exoRoot(new SceneNodeMaya(alembicFileAndTimeControl));
  exoRoot->type = SceneNode::SCENE_ROOT;
  exoRoot->dccIdentifier = dagPath.fullPathName().asChar();
  exoRoot->name = dagPath.partialPathName().asChar();
  for (unsigned int i = 0; i < dagPath.childCount(); ++i) {
    visitChild(dagPath.child(i), exoRoot, alembicFileAndTimeControl, replacer,
        allowDeformedByUs);
  }
  return exoRoot;
}
