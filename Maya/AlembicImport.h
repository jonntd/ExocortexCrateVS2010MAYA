#ifndef _ALEMBIC_IMPORT_H_
#define _ALEMBIC_IMPORT_H_

#include "AlembicObject.h"
#include "CommonImport.h"

class MayaProgressBar : public CommonProgressBar {
 public:
  void init(int min, int max, int incr);
  void start(void);
  void stop(void);
  void incr(int step);
  bool isCancelled(void);
};

class AlembicImportCommand : public MPxCommand {
 private:
  MStatus importSingleJob(const MString& job, int jobNumber);

 public:
  AlembicImportCommand(void);
  virtual ~AlembicImportCommand(void);

  virtual bool isUndoable(void) const { return false; }
  virtual MStatus doIt(const MArgList& args);

  static MSyntax createSyntax(void);
  static void* creator(void);
};

#endif
