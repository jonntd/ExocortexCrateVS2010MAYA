#include "stdafx.h"

#include <sstream>

#include "AlembicWriteJob.h"
#include "AlembicCamera.h"
#include "AlembicCurves.h"
#include "AlembicHair.h"
#include "AlembicImport.h"
#include "AlembicLicensing.h"
#include "AlembicObject.h"
#include "AlembicPoints.h"
#include "AlembicPolyMesh.h"
#include "AlembicSubD.h"
#include "AlembicXform.h"
#include "CommonLog.h"
#include "CommonUtilities.h"

#include <maya/MItDag.h>
#include "sceneGraph.h"

struct PreProcessStackElement {
  SceneNodePtr eNode;
  Abc::OObject oParent;

  PreProcessStackElement(SceneNodePtr enode, Abc::OObject parent)
      : eNode(enode), oParent(parent)
  {
  }
};

AlembicWriteJob::AlembicWriteJob(const MString &in_FileName,
                                 const MObjectArray &in_Selection,
                                 const MDoubleArray &in_Frames, bool use_ogawa,
                                 const std::vector<std::string> &in_prefixFilters,
                                 const std::set<std::string> &in_attributes,
                                 const std::vector<std::string> &in_userPrefixFilters,
                                 const std::set<std::string> &in_userAttributes)
    : useOgawa(use_ogawa),
    mPrefixFilters(in_prefixFilters),
    mAttributes(in_attributes),
    mUserPrefixFilters(in_userPrefixFilters),
    mUserAttributes(in_userAttributes)
{
  // ensure to clear the isRefAnimated cache
  clearIsRefAnimatedCache();

  mFileName = in_FileName;
  for (unsigned int i = 0; i < in_Selection.length(); i++) {
    mSelection.append(in_Selection[i]);
  }

  for (unsigned int i = 0; i < in_Frames.length(); i++) {
    mFrames.push_back(in_Frames[i]);
  }
}

AlembicWriteJob::~AlembicWriteJob() {}
void AlembicWriteJob::SetOption(const MString &in_Name, const MString &in_Value)
{
  ESS_PROFILE_SCOPE("AlembicWriteJob::SetOption");
  std::map<std::string, std::string>::iterator it =
      mOptions.find(in_Name.asChar());
  if (it == mOptions.end())
    mOptions.insert(std::pair<std::string, std::string>(in_Name.asChar(),
                                                        in_Value.asChar()));
  else {
    it->second = in_Value.asChar();
  }
}

bool AlembicWriteJob::HasOption(const MString &in_Name)
{
  std::map<std::string, std::string>::iterator it =
      mOptions.find(in_Name.asChar());
  return it != mOptions.end();
}

MString AlembicWriteJob::GetOption(const MString &in_Name)
{
  std::map<std::string, std::string>::iterator it =
      mOptions.find(in_Name.asChar());
  if (it != mOptions.end()) {
    return it->second.c_str();
  }
  return MString();
}

bool AlembicWriteJob::ObjectExists(const MObject &in_Ref)
{
  ESS_PROFILE_SCOPE("AlembicWriteJob::ObjectExists");

  std::string fullName(getFullNameFromRef(in_Ref).asChar());
  return mapObjects.find(fullName) != mapObjects.end();
}

bool AlembicWriteJob::AddObject(AlembicObjectPtr in_Obj)
{
  ESS_PROFILE_SCOPE("AlembicWriteJob::AddObject");
  if (!in_Obj) {
    return false;
  }
  const MObject &in_Ref = in_Obj->GetRef();
  if (in_Ref.isNull()) {
    return false;
  }

  const std::string fullName(getFullNameFromRef(in_Ref).asChar());
  mapObjects.insert(
      pairStrAbcObj(fullName, in_Obj));  // add it in the multi map!
  return true;
}

void AlembicWriteJob::createArchive(const char *sceneFileName)
{
  const std::string expName =
      getExporterName("Maya " EC_QUOTE(crate_Maya_Version));
  const std::string expFileName = getExporterFileName(sceneFileName);
  if (useOgawa) {
    mArchive = CreateArchiveWithInfo(
        Alembic::AbcCoreOgawa::WriteArchive(), mFileName.asChar(),
        expName.c_str(), expFileName.c_str(), Abc::ErrorHandler::kThrowPolicy);
  }
  else {
    mArchive = CreateArchiveWithInfo(
        Alembic::AbcCoreHDF5::WriteArchive(true), mFileName.asChar(),
        expName.c_str(), expFileName.c_str(), Abc::ErrorHandler::kThrowPolicy);
  }
}

MStatus AlembicWriteJob::PreProcess()
{
  ESS_PROFILE_SCOPE("AlembicWriteJob::PreProcess");
  // check filenames
  if (mFileName.length() == 0) {
    MGlobal::displayError("[ExocortexAlembic] No filename specified.");
    MPxCommand::setResult(
        "Error caught in AlembicWriteJob::PreProcess: no filename specified");
    return MStatus::kInvalidParameter;
  }

  // check objects
  if (mSelection.length() == 0) {
    MGlobal::displayError("[ExocortexAlembic] No objects specified.");
    MPxCommand::setResult(
        "Error caught in AlembicWriteJob::PreProcess: no objects specified");
    return MStatus::kInvalidParameter;
  }

  // check frames
  if (mFrames.size() == 0) {
    MGlobal::displayError("[ExocortexAlembic] No frames specified.");
    MPxCommand::setResult(
        "Error caught in AlembicWriteJob::PreProcess: no frame specified");
    return MStatus::kInvalidParameter;
  }

  // check if the file is currently in use
  if (getRefArchive(mFileName) > 0) {
    MGlobal::displayError("[ExocortexAlembic] Error writing to file '" +
                          mFileName + "'. File currently in use.");
    MPxCommand::setResult(
        "Error caught in AlembicWriteJob::PreProcess: no filename already in "
        "use");
    return MStatus::kInvalidParameter;
  }

  // init archive (use a locally scoped archive)
  // TODO: determine how to access the current maya scene path
  // MString sceneFileName = "Exported from:
  // "+Application().GetActiveProject().GetActiveScene().GetParameterValue("FileName").GetAsText();
  try {
    createArchive("Exported from Maya.");

    mTop = mArchive.getTop();

    // get the frame rate
    mFrameRate = MTime(1.0, MTime::kSeconds).as(MTime::uiUnit());
    const double timePerSample = 1.0 / mFrameRate;
    std::vector<AbcA::chrono_t> frames;
    for (LONG i = 0; i < mFrames.size(); i++) {
      frames.push_back(mFrames[i] * timePerSample);
    }

    // create the sampling
    if (frames.size() > 1) {
      const double timePerCycle = frames[frames.size() - 1] - frames[0];
      AbcA::TimeSamplingType samplingType((Abc::uint32_t)frames.size(),
                                          timePerCycle);
      AbcA::TimeSampling sampling(samplingType, frames);
      mTs = mArchive.addTimeSampling(sampling);
    }
    else {
      AbcA::TimeSampling sampling(1.0, frames[0]);
      mTs = mArchive.addTimeSampling(sampling);
    }
    Abc::OBox3dProperty boxProp = AbcG::CreateOArchiveBounds(mArchive, mTs);

    MDagPath dagPath;
    {
      MItDag().getPath(dagPath);
    }
    SceneNodePtr exoSceneRoot = buildMayaSceneGraph(dagPath, this->replacer);
    const bool bFlattenHierarchy = GetOption("flattenHierarchy") == "1";
    const bool bTransformCache = GetOption("transformCache") == "1";
    const bool bSelectChildren = false;
    {
      std::map<std::string, bool> selectionMap;
      for (int i = 0; i < (int)mSelection.length(); ++i) {
        MFnDagNode dagNode(mSelection[i]);
        selectionMap[dagNode.fullPathName().asChar()] = true;
      }
      selectNodes(exoSceneRoot, selectionMap,
                  !bFlattenHierarchy || bTransformCache, bSelectChildren,
                  !bTransformCache, true);
    }

    // create object for each
    MProgressWindow::reserve();
    MProgressWindow::setTitle("Alembic Export: Listing objects");
    MProgressWindow::setInterruptable(true);
    MProgressWindow::setProgressRange(0, mSelection.length());
    MProgressWindow::setProgress(0);

    MProgressWindow::startProgress();
    int interrupt = 20;
    bool processStopped = false;
    std::deque<PreProcessStackElement> sceneStack;

    sceneStack.push_back(PreProcessStackElement(exoSceneRoot, mTop));

    while (!sceneStack.empty()) {
      if (--interrupt == 0) {
        interrupt = 20;
        if (MProgressWindow::isCancelled()) {
          processStopped = true;
          break;
        }
      }

      PreProcessStackElement &sElement = sceneStack.back();
      SceneNodePtr eNode = sElement.eNode;
      sceneStack.pop_back();

      Abc::OObject oParent = sElement.oParent;
      Abc::OObject oNewParent;

      AlembicObjectPtr pNewObject;
      if (eNode->selected) {
        switch (eNode->type) {
          case SceneNode::SCENE_ROOT:
            break;
          case SceneNode::ITRANSFORM:
          case SceneNode::ETRANSFORM:
            pNewObject.reset(new AlembicXform(eNode, this, oParent));
            break;
          case SceneNode::CAMERA:
            pNewObject.reset(new AlembicCamera(eNode, this, oParent));
            break;
          case SceneNode::POLYMESH:
            pNewObject.reset(new AlembicPolyMesh(eNode, this, oParent));
            break;
          case SceneNode::SUBD:
            pNewObject.reset(new AlembicSubD(eNode, this, oParent));
            break;
          case SceneNode::CURVES:
            pNewObject.reset(new AlembicCurves(eNode, this, oParent));
            break;
          case SceneNode::PARTICLES:
            pNewObject.reset(new AlembicPoints(eNode, this, oParent));
            break;
          case SceneNode::HAIR:
            pNewObject.reset(new AlembicHair(eNode, this, oParent));
            break;
          default:
            ESS_LOG_WARNING("Unknown type: not exporting " << eNode->name);
        }
      }

      if (pNewObject) {
        AddObject(pNewObject);
        oNewParent = oParent.getChild(eNode->name);
      }
      else {
        oNewParent = oParent;
      }

      if (oNewParent.valid()) {
        for (std::list<SceneNodePtr>::iterator it = eNode->children.begin();
             it != eNode->children.end(); ++it) {
          if (!bFlattenHierarchy ||
              (bFlattenHierarchy && eNode->type == SceneNode::ETRANSFORM &&
               isShapeNode((*it)->type))) {
            // If flattening the hierarchy, we want to attach each external
            // transform to its corresponding geometry node.
            // All internal transforms should be skipped. Geometry nodes will
            // never have children (If and XSI geonode is parented
            // to another geonode, each will be parented to its extracted
            // transform node, and one node will be parented to the
            // transform of the other.
            sceneStack.push_back(PreProcessStackElement(*it, oNewParent));
          }
          else {
            // if we skip node A, we parent node A's children to the parent of A
            sceneStack.push_back(PreProcessStackElement(*it, oParent));
          }
        }
      }
      //*
      else {
        ESS_LOG_ERROR("Do not have reference to parent.");
        MPxCommand::setResult(
            "Error caught in AlembicWriteJob::PreProcess: do not have "
            "reference to parent");
        return MS::kFailure;
      }
      //*/
    }

    MProgressWindow::endProgress();
    return processStopped ? MStatus::kEndOfFile : MStatus::kSuccess;
  }
  catch (AbcU::Exception &e) {
    this->forceCloseArchive();
    MString exc(e.what());
    MGlobal::displayError("[ExocortexAlembic] Error writing to file '" +
                          mFileName + "' (" + exc +
                          "). Do you still have it opened?");
    MPxCommand::setResult(
        "Error caught in AlembicWriteJob::PreProcess: error writing file");
  }

  return MS::kFailure;
}

MStatus AlembicWriteJob::Process(double frame)
{
  ESS_PROFILE_SCOPE("AlembicWriteJob::Process");
  // find the right time frame!
  int i = -1;
  for (size_t j = 0; j < mFrames.size(); ++j) {
    // compare the frames
    if (fabs(mFrames[j] - frame) <= 0.001) {
      i = (int)j;
      break;
    }
  }

  if (i < 0) {
    return MS::kSuccess;
  }

  // run the export for all objects
  MayaProgressBar pBar;
  pBar.init(0, (int)mapObjects.size(), 1);
  pBar.start();

  int interrupt = 20;
  MStatus status;
  const double currentFrame = mFrames[i];
  const bool isFirstFrame = (i == 0);

  for (multiMapStrAbcObj::iterator it = mapObjects.begin();
       it != mapObjects.end(); ++it, --interrupt) {
    if (interrupt == 0) {
      interrupt = 20;
      if (pBar.isCancelled()) {
        break;
      }
      pBar.incr(20);
    }
    status = it->second->Save(currentFrame, mTs, isFirstFrame);
    if (status != MStatus::kSuccess) {
      MPxCommand::setResult("Error caught in AlembicWriteJob::Process: " +
                            status.errorString());
      break;
    }
  }
  pBar.stop();
  return status;
}

bool AlembicWriteJob::forceCloseArchive(void)
{
  if (mArchive.valid()) {
    mArchive.reset();
  }
  return true;
}

AlembicExportCommand::AlembicExportCommand() {}
AlembicExportCommand::~AlembicExportCommand() {}
MSyntax AlembicExportCommand::createSyntax()
{
  MSyntax syntax;
  syntax.addFlag("-h", "-help");
  syntax.addFlag("-j", "-jobArg", MSyntax::kString);
  syntax.makeFlagMultiUse("-j");
  syntax.enableQuery(false);
  syntax.enableEdit(false);

  return syntax;
}

void *AlembicExportCommand::creator() { return new AlembicExportCommand(); }
static void restoreOldTime(MTime &start, MTime &end, MTime &curr, MTime &minT,
                           MTime &maxT)
{
  const double st = start.as(start.unit()), en = end.as(end.unit()),
               cu = curr.as(curr.unit()), mi = minT.as(minT.unit()),
               ma = maxT.as(maxT.unit());

  MString inst = "playbackOptions -ast ";
  inst += st;
  inst += " -aet ";
  inst += en;
  inst += " -min ";
  inst += mi;
  inst += " -max ";
  inst += ma;
  inst += ";\ncurrentTime ";
  inst += cu;
  inst += ";";

  MGlobal::executeCommand(inst);
}

template <typename T1>
void splitListArg(const MString &inList, T1 &outList)
{
  std::istringstream iss(inList.asChar());
  std::string item;
  while (std::getline(iss, item, ',')) {
    outList.insert(outList.end(), item);
  }
}

MStatus AlembicExportCommand::doIt(const MArgList &args)
{
  ESS_PROFILE_SCOPE("AlembicExportCommand::doIt");

  MStatus status = MS::kFailure;

  MTime currentAnimStartTime = MAnimControl::animationStartTime(),
        currentAnimEndTime = MAnimControl::animationEndTime(),
        oldCurTime = MAnimControl::currentTime(),
        curMinTime = MAnimControl::minTime(),
        curMaxTime = MAnimControl::maxTime();
  MArgParser argData(syntax(), args, &status);

  if (argData.isFlagSet("help")) {
    // TODO: implement help for this command
    // MGlobal::displayInfo(util::getHelpText());
    return MS::kSuccess;
  }

  unsigned int jobCount = argData.numberOfFlagUses("jobArg");
  MStringArray jobStrings;
  if (jobCount == 0) {
    // TODO: display dialog
    MGlobal::displayError("[ExocortexAlembic] No jobs specified.");
    MPxCommand::setResult(
        "Error caught in AlembicExportCommand::doIt: no job specified");
    return status;
  }
  else {
    // get all of the jobstrings
    for (unsigned int i = 0; i < jobCount; i++) {
      MArgList jobArgList;
      argData.getFlagArgumentList("jobArg", i, jobArgList);
      jobStrings.append(jobArgList.asString(0));
    }
  }

  // create a vector to store the jobs
  std::vector<AlembicWriteJob *> jobPtrs;
  double minFrame = 1000000.0;
  double maxFrame = -1000000.0;
  double maxSteps = 1;
  double maxSubsteps = 1;

  // init the curve accumulators
  AlembicCurveAccumulator::Initialize();

  try {
    // for each job, check the arguments
    bool failure = false;
    for (unsigned int i = 0; i < jobStrings.length(); ++i) {
      double frameIn = 1.0;
      double frameOut = 1.0;
      double frameSteps = 1.0;
      double frameSubSteps = 1.0;
      MString filename;
      bool purepointcache = false;
      bool normals = true;
      bool uvs = true;
      bool facesets = true;
      bool bindpose = true;
      bool dynamictopology = false;
      bool globalspace = false;
      bool withouthierarchy = false;
      bool transformcache = false;
      bool useInitShadGrp = false;
      bool useOgawa = false;  // Later, will need to be changed!

      MStringArray objectStrings;
      std::vector<std::string> prefixFilters;
      std::set<std::string> attributes;
      std::vector<std::string> userPrefixFilters;
      std::set<std::string> userAttributes;
      MObjectArray objects;
      std::string search_str, replace_str;

      // process all tokens of the job
      MStringArray tokens;
      jobStrings[i].split(';', tokens);
      for (unsigned int j = 0; j < tokens.length(); j++) {
        MStringArray valuePair;
        tokens[j].split('=', valuePair);
        if (valuePair.length() != 2) {
          MGlobal::displayWarning(
              "[ExocortexAlembic] Skipping invalid token: " + tokens[j]);
          continue;
        }

        const MString &lowerValue = valuePair[0].toLowerCase();
        if (lowerValue == "in") {
          frameIn = valuePair[1].asDouble();
        }
        else if (lowerValue == "out") {
          frameOut = valuePair[1].asDouble();
        }
        else if (lowerValue == "step") {
          frameSteps = valuePair[1].asDouble();
        }
        else if (lowerValue == "substep") {
          frameSubSteps = valuePair[1].asDouble();
        }
        else if (lowerValue == "normals") {
          normals = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "uvs") {
          uvs = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "facesets") {
          facesets = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "bindpose") {
          bindpose = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "purepointcache") {
          purepointcache = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "dynamictopology") {
          dynamictopology = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "globalspace") {
          globalspace = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "withouthierarchy") {
          withouthierarchy = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "transformcache") {
          transformcache = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "filename") {
          filename = valuePair[1];
        }
        else if (lowerValue == "objects") {
          // try to find each object
          valuePair[1].split(',', objectStrings);
        }
        else if (lowerValue == "useinitshadgrp") {
          useInitShadGrp = valuePair[1].asInt() != 0;
        }

        // search/replace
        else if (lowerValue == "search") {
          search_str = valuePair[1].asChar();
        }
        else if (lowerValue == "replace") {
          replace_str = valuePair[1].asChar();
        }
        else if (lowerValue == "ogawa") {
          useOgawa = valuePair[1].asInt() != 0;
        }
        else if (lowerValue == "attrprefixes") {
          splitListArg(valuePair[1], prefixFilters);
        }
        else if (lowerValue == "attrs") {
          splitListArg(valuePair[1], attributes);
        }
        else if (lowerValue == "userattrprefixes") {
          splitListArg(valuePair[1], userPrefixFilters);
        }
        else if (lowerValue == "userattrs") {
          splitListArg(valuePair[1], userAttributes);
        }
        else {
          MGlobal::displayWarning(
              "[ExocortexAlembic] Skipping invalid token: " + tokens[j]);
          continue;
        }
      }

      // now check the object strings
      for (unsigned int k = 0; k < objectStrings.length(); k++) {
        MSelectionList sl;
        MString objectString = objectStrings[k];
        sl.add(objectString);
        MDagPath dag;
        for (unsigned int l = 0; l < sl.length(); l++) {
          sl.getDagPath(l, dag);
          MObject objRef = dag.node();
          if (objRef.isNull()) {
            MGlobal::displayWarning("[ExocortexAlembic] Skipping object '" +
                                    objectStrings[k] + "', not found.");
            break;
          }

          // get all parents
          MObjectArray parents;

          // check if this is a camera
          bool isCamera = false;
          for (unsigned int m = 0; m < dag.childCount(); ++m) {
            MFnDagNode child(dag.child(m));
            MFn::Type ctype = child.object().apiType();
            if (ctype == MFn::kCamera) {
              isCamera = true;
              break;
            }
          }

          if (dag.node().apiType() == MFn::kTransform && !isCamera &&
              !globalspace && !withouthierarchy) {
            MDagPath ppath = dag;
            while (!ppath.node().isNull() && ppath.length() > 0 &&
                   ppath.isValid()) {
              parents.append(ppath.node());
              if (ppath.pop() != MStatus::kSuccess) {
                break;
              }
            }
          }
          else {
            parents.append(dag.node());
          }

          // push all parents in
          while (parents.length() > 0) {
            bool found = false;
            for (unsigned int m = 0; m < objects.length(); m++) {
              if (objects[m] == parents[parents.length() - 1]) {
                found = true;
                break;
              }
            }
            if (!found) {
              objects.append(parents[parents.length() - 1]);
            }
            parents.remove(parents.length() - 1);
          }

          // check all of the shapes below
          if (!transformcache) {
            sl.getDagPath(l, dag);
            for (unsigned int m = 0; m < dag.childCount(); m++) {
              MFnDagNode child(dag.child(m));
              if (child.isIntermediateObject()) {
                continue;
              }
              objects.append(child.object());
            }
          }
        }
      }

      // check if we have incompatible subframes
      if (maxSubsteps > 1.0 && frameSubSteps > 1.0) {
        const double part = (frameSubSteps > maxSubsteps)
                                ? (frameSubSteps / maxSubsteps)
                                : (maxSubsteps / frameSubSteps);
        if (abs(part - floor(part)) > 0.001) {
          MString frameSubStepsStr, maxSubstepsStr;
          frameSubStepsStr.set(frameSubSteps);
          maxSubstepsStr.set(maxSubsteps);
          MGlobal::displayError(
              "[ExocortexAlembic] You cannot combine substeps " +
              frameSubStepsStr + " and " + maxSubstepsStr +
              " in one export. Aborting.");
          return MStatus::kInvalidParameter;
        }
      }

      // remember the min and max values for the frames
      if (frameIn < minFrame) {
        minFrame = frameIn;
      }
      if (frameOut > maxFrame) {
        maxFrame = frameOut;
      }
      if (frameSteps > maxSteps) {
        maxSteps = frameSteps;
      }
      if (frameSteps > 1.0) {
        frameSubSteps = 1.0;
      }
      if (frameSubSteps > maxSubsteps) {
        maxSubsteps = frameSubSteps;
      }

      // check if we have a filename
      if (filename.length() == 0) {
        MGlobal::displayError("[ExocortexAlembic] No filename specified.");
        for (size_t k = 0; k < jobPtrs.size(); k++) {
          delete (jobPtrs[k]);
        }
        MPxCommand::setResult(
            "Error caught in AlembicExportCommand::doIt: no filename "
            "specified");
        return MStatus::kFailure;
      }

      // construct the frames
      MDoubleArray frames;
      {
        const double frameIncr = frameSteps / frameSubSteps;
        for (double frame = frameIn; frame <= frameOut; frame += frameIncr) {
          frames.append(frame);
        }
      }

      AlembicWriteJob *job =
          new AlembicWriteJob(filename, objects, frames, useOgawa,
              prefixFilters, attributes, userPrefixFilters, userAttributes);
      job->SetOption("exportNormals", normals ? "1" : "0");
      job->SetOption("exportUVs", uvs ? "1" : "0");
      job->SetOption("exportFaceSets", facesets ? "1" : "0");
      job->SetOption("exportInitShadGrp", useInitShadGrp ? "1" : "0");
      job->SetOption("exportBindPose", bindpose ? "1" : "0");
      job->SetOption("exportPurePointCache", purepointcache ? "1" : "0");
      job->SetOption("exportDynamicTopology", dynamictopology ? "1" : "0");
      job->SetOption("indexedNormals", "1");
      job->SetOption("indexedUVs", "1");
      job->SetOption("exportInGlobalSpace", globalspace ? "1" : "0");
      job->SetOption("flattenHierarchy", withouthierarchy ? "1" : "0");
      job->SetOption("transformCache", transformcache ? "1" : "0");

      // check if the search/replace strings are valid!
      if (search_str.length() ? !replace_str.length()
                              : replace_str.length())  // either search or
                                                       // replace string is
                                                       // missing or empty!
      {
        ESS_LOG_WARNING(
            "Missing search or replace parameter. No strings will be "
            "replaced.");
        job->replacer = SearchReplace::createReplacer();
      }
      else {
        job->replacer = SearchReplace::createReplacer(search_str, replace_str);
      }

      // check if the job is satifsied
      if (job->PreProcess() != MStatus::kSuccess) {
        MGlobal::displayError("[ExocortexAlembic] Job skipped. Not satisfied.");
        delete (job);
        failure = true;
        break;
      }

      // push the job to our registry
      MGlobal::displayInfo("[ExocortexAlembic] Using WriteJob:" +
                           jobStrings[i]);
      jobPtrs.push_back(job);
    }

    if (failure) {
      for (size_t k = 0; k < jobPtrs.size(); k++) {
        delete (jobPtrs[k]);
      }
      return MS::kFailure;
    }

    // compute the job count
    unsigned int jobFrameCount = 0;
    for (size_t i = 0; i < jobPtrs.size(); i++)
      jobFrameCount += (unsigned int)jobPtrs[i]->GetNbObjects() *
                       (unsigned int)jobPtrs[i]->GetFrames().size();

    // now, let's run through all frames, and process the jobs
    const double frameRate = MTime(1.0, MTime::kSeconds).as(MTime::uiUnit());
    const double incrSteps = maxSteps / maxSubsteps;
    double nextFrame = minFrame + incrSteps;

    for (double frame = minFrame; frame <= maxFrame;
         frame += incrSteps, nextFrame += incrSteps) {
      MAnimControl::setCurrentTime(MTime(frame / frameRate, MTime::kSeconds));
      MAnimControl::setAnimationEndTime(
          MTime(nextFrame / frameRate, MTime::kSeconds));
      MAnimControl::playForward();  // this way, it forces Maya to play exactly
      // one frame! and particles are updated!

      AlembicCurveAccumulator::StartRecordingFrame();
      for (size_t i = 0; i < jobPtrs.size(); i++) {
        MStatus status = jobPtrs[i]->Process(frame);
        if (status != MStatus::kSuccess) {
          MGlobal::displayError("[ExocortexAlembic] Job aborted :" +
                                jobPtrs[i]->GetFileName());
          for (size_t k = 0; k < jobPtrs.size(); k++) {
            delete (jobPtrs[k]);
          }
          restoreOldTime(currentAnimStartTime, currentAnimEndTime, oldCurTime,
                         curMinTime, curMaxTime);
          return status;
        }
      }
      AlembicCurveAccumulator::StopRecordingFrame();
    }
  }
  catch (...) {
    MGlobal::displayError(
        "[ExocortexAlembic] Jobs aborted, force closing all archives!");
    for (std::vector<AlembicWriteJob *>::iterator beg = jobPtrs.begin();
         beg != jobPtrs.end(); ++beg) {
      (*beg)->forceCloseArchive();
    }
    restoreOldTime(currentAnimStartTime, currentAnimEndTime, oldCurTime,
                   curMinTime, curMaxTime);
    MPxCommand::setResult("Error caught in AlembicExportCommand::doIt");
    status = MS::kFailure;
  }
  MAnimControl::stop();
  AlembicCurveAccumulator::Destroy();

  // restore the animation start/end time and the current time!
  restoreOldTime(currentAnimStartTime, currentAnimEndTime, oldCurTime,
                 curMinTime, curMaxTime);

  // delete all jobs
  for (size_t k = 0; k < jobPtrs.size(); k++) {
    delete (jobPtrs[k]);
  }

  // remove all known archives
  deleteAllArchives();
  return status;
}
