// $Id$

// Copyright by Johannes Sixt
// This file is under GPL, the GNU General Public Licence

#include "debugger.h"
#include "dbgdriver.h"
#include "pgmargs.h"
#include "procattach.h"
#include "typetable.h"
#include "exprwnd.h"
#include "valarray.h"
#include <qregexp.h>
#include <qfileinfo.h>
#include <qlistbox.h>
#include <kapp.h>
#include <ksimpleconfig.h>
#include <kconfig.h>
#include <kwm.h>
#if QT_VERSION >= 200
#include <klocale.h>			/* i18n */
#include <kmessagebox.h>
#else
#include <kmsgbox.h>
#endif
#include <ctype.h>
#include <stdlib.h>			/* strtol, atoi */
#ifdef HAVE_UNISTD_H
#include <unistd.h>			/* sleep(3) */
#endif
#include "mydebug.h"


KDebugger::KDebugger(QWidget* parent,
		     ExprWnd* localVars,
		     ExprWnd* watchVars,
		     QListBox* backtrace,
		     DebuggerDriver* driver
		     ) :
	QObject(parent, "debugger"),
	m_haveExecutable(false),
	m_programActive(false),
	m_programRunning(false),
	m_sharedLibsListed(false),
	m_runRedirect(0),
	m_typeTable(0),
	m_programConfig(0),
	m_d(driver),
	m_localVariables(*localVars),
	m_watchVariables(*watchVars),
	m_btWindow(*backtrace),
	m_animationTimer(this),
	m_animationInterval(0)
{
    m_envVars.setAutoDelete(true);

    connect(&m_localVariables, SIGNAL(expanding(KTreeViewItem*,bool&)),
	    SLOT(slotLocalsExpanding(KTreeViewItem*,bool&)));
    connect(&m_watchVariables, SIGNAL(expanding(KTreeViewItem*,bool&)),
	    SLOT(slotWatchExpanding(KTreeViewItem*,bool&)));

    connect(&m_btWindow, SIGNAL(highlighted(int)), SLOT(gotoFrame(int)));

    connect(m_d, SIGNAL(activateFileLine(const QString&,int)),
	    this, SIGNAL(activateFileLine(const QString&,int)));

    // debugger process
    connect(m_d, SIGNAL(processExited(KProcess*)), SLOT(gdbExited(KProcess*)));
    connect(m_d, SIGNAL(commandReceived(CmdQueueItem*,const char*)),
	    SLOT(parse(CmdQueueItem*,const char*)));
    // we shouldn't do this, it's very unsafe (different arg lists):
    connect(m_d, SIGNAL(receivedStdout(KProcess*,char*,int)),
	    SIGNAL(updateUI()));
    connect(m_d, SIGNAL(wroteStdin(KProcess*)), SIGNAL(updateUI()));
    connect(m_d, SIGNAL(inferiorRunning()), SLOT(slotInferiorRunning()));
    connect(m_d, SIGNAL(enterIdleState()), SLOT(backgroundUpdate()));

    // animation
    connect(&m_animationTimer, SIGNAL(timeout()), SIGNAL(animationTimeout()));
    // special update of animation
    connect(this, SIGNAL(updateUI()), SLOT(slotUpdateAnimation()));

    emit updateUI();
}

KDebugger::~KDebugger()
{
    if (m_programConfig != 0) {
	saveProgramSettings();
	m_programConfig->sync();
	delete m_programConfig;
    }
    // delete breakpoint objects
    for (int i = m_brkpts.size()-1; i >= 0; i--) {
	delete m_brkpts[i];
    }

    delete m_typeTable;
}


void KDebugger::saveSettings(KConfig* /*config*/)
{
}

void KDebugger::restoreSettings(KConfig* /*config*/)
{
}


//////////////////////////////////////////////////////////////////////
// external interface

const char GeneralGroup[] = "General";
const char DebuggerCmdStr[] = "DebuggerCmdStr";

bool KDebugger::debugProgram(const QString& name)
{
    if (m_d->isRunning())
    {
	QApplication::setOverrideCursor(waitCursor);

	stopGdb();
	/*
	 * We MUST wait until the slot gdbExited() has been called. But to
	 * avoid a deadlock, we wait only for some certain maximum time.
	 * Should this timeout be reached, the only reasonable thing one
	 * could do then is exiting kdbg.
	 */
	int maxTime = 20;		/* about 20 seconds */
	while (m_haveExecutable && maxTime > 0) {
	    kapp->processEvents(1000);
	    // give gdb time to die (and send a SIGCLD)
	    ::sleep(1);
	    --maxTime;
	}

	QApplication::restoreOverrideCursor();

	if (m_d->isRunning() || m_haveExecutable) {
	    /* timed out! We can't really do anything useful now */
	    TRACE("timed out while waiting for gdb to die!");
	    return false;
	}
    }

    // create the program settings object
    openProgramConfig(name);

    // get debugger command from per-program settings
    if (m_programConfig != 0) {
	m_programConfig->setGroup(GeneralGroup);
	m_debuggerCmd = m_programConfig->readEntry(DebuggerCmdStr);
    }
    // the rest is read in later in the handler of DCexecutable

    if (!startGdb()) {
	TRACE("startGdb failed");
	return false;
    }

    TRACE("before file cmd");
    m_d->executeCmd(DCexecutable, name);
    m_executable = name;

    // set remote target
    if (!m_remoteDevice.isEmpty()) {
	m_d->executeCmd(DCtargetremote, m_remoteDevice);
	m_d->queueCmd(DCbt, DebuggerDriver::QMoverride);
	m_d->queueCmd(DCframe, 0, DebuggerDriver::QMnormal);
	m_programActive = true;
	m_haveExecutable = true;
    }

    // create a type table
    m_typeTable = new ProgramTypeTable;
    m_sharedLibsListed = false;

    emit updateUI();

    return true;
}

void KDebugger::useCoreFile(QString corefile, bool batch)
{
    m_corefile = corefile;
    if (!batch) {
	CmdQueueItem* cmd = loadCoreFile();
	cmd->m_byUser = true;
    }
}

void KDebugger::programRun()
{
    if (!isReady())
	return;

    // when program is active, but not a core file, continue
    // otherwise run the program
    if (m_programActive && m_corefile.isEmpty()) {
	// gdb command: continue
	m_d->executeCmd(DCcont, true);
    } else {
	// gdb command: run
	m_d->executeCmd(DCrun, m_runRedirect, true);
	m_corefile = QString();
	m_programActive = true;
    }
    m_programRunning = true;
}

void KDebugger::programAttach()
{
    if (!isReady())
	return;

    ProcAttach dlg(parentWidget());
    dlg.setText(m_attachedPid);
    if (dlg.exec()) {
	m_attachedPid = dlg.text();
	TRACE("Attaching to " + m_attachedPid);
	m_d->executeCmd(DCattach, m_attachedPid);
	m_programActive = true;
	m_programRunning = true;
    }
}

void KDebugger::programRunAgain()
{
    if (canSingleStep()) {
	m_d->executeCmd(DCrun, m_runRedirect, true);
	m_corefile = QString();
	m_programRunning = true;
    }
}

void KDebugger::programStep()
{
    if (canSingleStep()) {
	m_d->executeCmd(DCstep, true);
	m_programRunning = true;
    }
}

void KDebugger::programNext()
{
    if (canSingleStep()) {
	m_d->executeCmd(DCnext, true);
	m_programRunning = true;
    }
}

void KDebugger::programFinish()
{
    if (canSingleStep()) {
	m_d->executeCmd(DCfinish, true);
	m_programRunning = true;
    }
}

void KDebugger::programKill()
{
    if (haveExecutable() && isProgramActive()) {
	if (m_programRunning) {
	    m_d->interruptInferior();
	}
	// this is an emergency command; flush queues
	m_d->flushCommands(true);
	m_d->executeCmd(DCkill, true);
    }
}

bool KDebugger::runUntil(const QString& fileName, int lineNo)
{
    if (isReady() && m_programActive && !m_programRunning) {
	// strip off directory part of file name
	QString file = fileName;
#if QT_VERSION < 200
	file.detach();
#endif
	int offset = file.findRev("/");
	if (offset >= 0) {
	    file.remove(0, offset+1);
	}
	m_d->executeCmd(DCuntil, file, lineNo, true);
	m_programRunning = true;
	return true;
    } else {
	return false;
    }
}

void KDebugger::programBreak()
{
    if (m_haveExecutable && m_programRunning) {
	m_d->interruptInferior();
    }
}

void KDebugger::programArgs()
{
    if (m_haveExecutable) {
	PgmArgs dlg(parentWidget(), m_executable, m_envVars);
	dlg.setArgs(m_programArgs);
	dlg.setWd(m_programWD);
	if (dlg.exec()) {
	    updateProgEnvironment(dlg.args(), dlg.wd(), dlg.envVars());
	}
    }
}

bool KDebugger::setBreakpoint(QString file, int lineNo, bool temporary)
{
    if (!isReady()) {
	return false;
    }

    Breakpoint* bp = breakpointByFilePos(file, lineNo);
    if (bp == 0)
    {
	// no such breakpoint, so set a new one
	// strip off directory part of file name
#if QT_VERSION < 200
	file.detach();
#endif
	int offset = file.findRev("/");
	if (offset >= 0) {
	    file.remove(0, offset+1);
	}
	m_d->executeCmd(temporary ? DCtbreakline : DCbreakline,
			file, lineNo);
    }
    else
    {
	/*
	 * If the breakpoint is disabled, enable it; if it's enabled,
	 * delete that breakpoint.
	 */
	if (bp->enabled) {
	    m_d->executeCmd(DCdelete, bp->id);
	} else {
	    m_d->executeCmd(DCenable, bp->id);
	}
    }
    return true;
}

bool KDebugger::enableDisableBreakpoint(QString file, int lineNo)
{
    if (!isReady()) {
	return false;
    }

    Breakpoint* bp = breakpointByFilePos(file, lineNo);
    if (bp == 0)
	return true;

    // toggle enabled/disabled state
    if (bp->enabled) {
	m_d->executeCmd(DCdisable, bp->id);
    } else {
	m_d->executeCmd(DCenable, bp->id);
    }
    return true;
}

bool KDebugger::canSingleStep()
{
    return isReady() && m_programActive && !m_programRunning;
}

bool KDebugger::canChangeBreakpoints()
{
    return isReady() && !m_programRunning;
}

bool KDebugger::isReady() const 
{
    return m_haveExecutable &&
	m_d->canExecuteImmediately();
}

bool KDebugger::isIdle() const
{
    return m_d->isIdle();
}


//////////////////////////////////////////////////////////
// debugger driver

bool KDebugger::startGdb()
{
    emit debuggerStarting();

    /*
     * If the per-program command string is empty, use the global setting
     * (which might also be empty, in which case the driver uses its
     * default).
     */
    QString debuggerCmd = m_debuggerCmd.isEmpty()  ?
	m_generalDebuggerCmd  :  m_debuggerCmd;
    m_explicitKill = false;
    if (!m_d->startup(debuggerCmd)) {
	return false;
    }

    /*
     * If we have an output terminal, we use it. Otherwise we will run the
     * program with input and output redirected to /dev/null.
     */
    m_runRedirect = 0;
    if (!m_inferiorTerminal.isEmpty()) {
	m_d->executeCmd(DCtty, m_inferiorTerminal);
    } else {
	m_runRedirect = RDNstdin|RDNstdout|RDNstderr;
    }

    return true;
}

void KDebugger::stopGdb()
{
    m_explicitKill = true;
    m_d->terminate();
}

void KDebugger::gdbExited(KProcess*)
{
    /*
     * Save settings, but only if gdb has already processed "info line
     * main", otherwise we would save an empty config file, because it
     * isn't read in until then!
     */
    if (m_programConfig != 0) {
	if (m_haveExecutable) {
	    saveProgramSettings();
	    m_programConfig->sync();
	}
	delete m_programConfig;
	m_programConfig = 0;
    }

    // erase types
    delete m_typeTable;
    m_typeTable = 0;

    if (m_explicitKill) {
	TRACE("gdb exited normally");
    } else {
	QString msg = i18n("gdb exited unexpectedly.\n"
			   "Restart the session (e.g. with File|Executable).");
#if QT_VERSION < 200
	KMsgBox::message(parentWidget(), kapp->appName(), msg, KMsgBox::EXCLAMATION);
#else
	KMessageBox::error(parentWidget(), msg);
#endif
    }

    // reset state
    m_haveExecutable = false;
    m_executable = "";
    m_programActive = false;
    m_programRunning = false;
    m_explicitKill = false;
    m_debuggerCmd = QString();		/* use global setting at next start! */

    // stop gear wheel and erase PC
    stopAnimation();
    emit updatePC(QString(), -1, 0);
}

void KDebugger::openProgramConfig(const QString& name)
{
    ASSERT(m_programConfig = 0);

    QFileInfo fi(name);
    QString pgmConfigFile = fi.dirPath(true);
    if (!pgmConfigFile.isEmpty()) {
	pgmConfigFile += '/';
    }
    pgmConfigFile += ".kdbgrc." + fi.fileName();
    TRACE("program config file = " + pgmConfigFile);
    // check whether we can write to the file
    QFile file(pgmConfigFile);
    bool readonly = true;
    bool openit = true;
    if (file.open(IO_ReadWrite)) {	/* don't truncate! */
	readonly = false;
	// the file exists now
    } else if (!file.open(IO_ReadOnly)) {
	/* file does not exist and cannot be created: don't use it */
	openit = false;
    }
    if (openit) {
	m_programConfig = new KSimpleConfig(pgmConfigFile, readonly);
    }
}

const char EnvironmentGroup[] = "Environment";
const char WatchGroup[] = "Watches";
const char FileVersion[] = "FileVersion";
const char ProgramArgs[] = "ProgramArgs";
const char WorkingDirectory[] = "WorkingDirectory";
const char Variable[] = "Var%d";
const char Value[] = "Value%d";
const char ExprFmt[] = "Expr%d";

void KDebugger::saveProgramSettings()
{
    ASSERT(m_programConfig != 0);
    m_programConfig->setGroup(GeneralGroup);
    m_programConfig->writeEntry(FileVersion, 1);
    m_programConfig->writeEntry(ProgramArgs, m_programArgs);
    m_programConfig->writeEntry(WorkingDirectory, m_programWD);
    m_programConfig->writeEntry(DebuggerCmdStr, m_debuggerCmd);

    // write environment variables
    m_programConfig->deleteGroup(EnvironmentGroup);
    m_programConfig->setGroup(EnvironmentGroup);
    QDictIterator<EnvVar> it = m_envVars;
    EnvVar* var;
    QString varName;
    QString varValue;
    for (int i = 0; (var = it) != 0; ++it, ++i) {
	varName.sprintf(Variable, i);
	varValue.sprintf(Value, i);
	m_programConfig->writeEntry(varName, it.currentKey());
	m_programConfig->writeEntry(varValue, var->value);
    }

    saveBreakpoints(m_programConfig);

    // watch expressions
    // first get rid of whatever was in this group
    m_programConfig->deleteGroup(WatchGroup);
    // then start a new group
    m_programConfig->setGroup(WatchGroup);
    KTreeViewItem* item = m_watchVariables.itemAt(0);
    int watchNum = 0;
    for (; item != 0; item = item->getSibling(), ++watchNum) {
	varName.sprintf(ExprFmt, watchNum);
	m_programConfig->writeEntry(varName, item->getText());
    }
}

void KDebugger::restoreProgramSettings()
{
    ASSERT(m_programConfig != 0);
    m_programConfig->setGroup(GeneralGroup);
    /*
     * We ignore file version for now we will use it in the future to
     * distinguish different versions of this configuration file.
     */
    m_debuggerCmd = m_programConfig->readEntry(DebuggerCmdStr);
    QString pgmArgs = m_programConfig->readEntry(ProgramArgs);
    QString pgmWd = m_programConfig->readEntry(WorkingDirectory);

    // read environment variables
    m_programConfig->setGroup(EnvironmentGroup);
    m_envVars.clear();
    QDict<EnvVar> pgmVars;
    EnvVar* var;
    QString varName;
    QString varValue;
    for (int i = 0;; ++i) {
	varName.sprintf(Variable, i);
	varValue.sprintf(Value, i);
	if (!m_programConfig->hasKey(varName)) {
	    /* entry not present, assume that we've hit them all */
	    break;
	}
	QString name = m_programConfig->readEntry(varName);
	if (name.isEmpty()) {
	    // skip empty names
	    continue;
	}
	var = new EnvVar;
	var->value = m_programConfig->readEntry(varValue);
	var->status = EnvVar::EVnew;
	pgmVars.insert(name, var);
    }

    updateProgEnvironment(pgmArgs, pgmWd, pgmVars);

    restoreBreakpoints(m_programConfig);

    // watch expressions
    m_programConfig->setGroup(WatchGroup);
    m_watchVariables.clear();
    for (int i = 0;; ++i) {
	varName.sprintf(ExprFmt, i);
	if (!m_programConfig->hasKey(varName)) {
	    /* entry not present, assume that we've hit them all */
	    break;
	}
	QString expr = m_programConfig->readEntry(varName);
	if (expr.isEmpty()) {
	    // skip empty expressions
	    continue;
	}
	addWatch(expr);
    }
}

/*
 * Breakpoints are saved one per group.
 */
const char BPGroup[] = "Breakpoint %d";
const char File[] = "File";
const char Line[] = "Line";
const char Temporary[] = "Temporary";
const char Enabled[] = "Enabled";
const char Condition[] = "Condition";

void KDebugger::saveBreakpoints(KSimpleConfig* config)
{
    QString groupName;
    int i;
    for (i = 0; uint(i) < m_brkpts.size(); i++) {
	groupName.sprintf(BPGroup, i);
	config->setGroup(groupName);
	Breakpoint* bp = m_brkpts[i];
	config->writeEntry(File, bp->fileName);
	config->writeEntry(Line, bp->lineNo);
	config->writeEntry(Temporary, bp->temporary);
	config->writeEntry(Enabled, bp->enabled);
	if (bp->condition.isEmpty())
	    config->deleteEntry(Condition, false);
	else
	    config->writeEntry(Condition, bp->condition);
	// we do not save the ignore count
    }
    // delete remaining groups
    // we recognize that a group is present if there is an Enabled entry
    for (;; i++) {
	groupName.sprintf(BPGroup, i);
	config->setGroup(groupName);
	if (!config->hasKey(Enabled)) {
	    /* group not present, assume that we've hit them all */
	    break;
	}
	config->deleteGroup(groupName);
    }
}

void KDebugger::restoreBreakpoints(KSimpleConfig* config)
{
    QString groupName;
    QString fileName;
    int lineNo;
    bool enabled, temporary;
    QString condition;
    /*
     * We recognize the end of the list if there is no Enabled entry
     * present.
     */
    for (int i = 0;; i++) {
	groupName.sprintf(BPGroup, i);
	config->setGroup(groupName);
	if (!config->hasKey(Enabled)) {
	    /* group not present, assume that we've hit them all */
	    break;
	}
	fileName = config->readEntry(File);
	lineNo = config->readNumEntry(Line, -1);
	if (lineNo < 0 || fileName.isEmpty())
	    continue;
	enabled = config->readBoolEntry(Enabled, true);
	temporary = config->readBoolEntry(Temporary, false);
	condition = config->readEntry(Condition);
	/*
	 * Add the breakpoint. We assume that we have started a new
	 * instance of gdb, because we assign the breakpoint ids ourselves,
	 * starting with 1. Then we use this id to disable the breakpoint,
	 * if necessary. If this assignment of ids doesn't work, (maybe
	 * because this isn't a fresh gdb at all), we disable the wrong
	 * breakpoint! Oh well... for now it works.
	 */
	m_d->executeCmd(temporary ? DCtbreakline : DCbreakline,
			fileName, lineNo);
	if (!enabled) {
	    m_d->executeCmd(DCdisable, i+1);
	}
	if (!condition.isEmpty()) {
	    m_d->executeCmd(DCcondition, condition, i+1);
	}
    }
    m_d->queueCmd(DCinfobreak, DebuggerDriver::QMoverride);
}


// parse output of command cmd
void KDebugger::parse(CmdQueueItem* cmd, const char* output)
{
    ASSERT(cmd != 0);			/* queue mustn't be empty */

    TRACE(QString(__PRETTY_FUNCTION__) + " parsing " + output);

    switch (cmd->m_cmd) {
    case DCtargetremote:
	// the output (if any) is uninteresting
    case DCsetargs:
    case DCtty:
	// there is no output
    case DCsetenv:
    case DCunsetenv:
	/* if value is empty, we see output, but we don't care */
	break;
    case DCcd:
	/* display gdb's message in the status bar */
	m_d->parseChangeWD(output, m_statusMessage);
	emit updateStatusMessage();
	break;
    case DCinitialize:
	break;
    case DCexecutable:
	if (m_d->parseChangeExecutable(output, m_statusMessage))
	{
	    // success; restore breakpoints etc.
	    if (m_programConfig != 0) {
		restoreProgramSettings();
	    }
	    // load file containing main() or core file
	    if (m_corefile.isEmpty()) {
		if (m_remoteDevice.isEmpty())
		    m_d->queueCmd(DCinfolinemain, DebuggerDriver::QMnormal);
	    } else {
		// load core file
		loadCoreFile();
	    }
	    if (!m_statusMessage.isEmpty())
		emit updateStatusMessage();
	} else {
	    QString msg = "gdb: " + m_statusMessage;
#if QT_VERSION < 200
	    KMsgBox::message(parentWidget(), kapp->appName(), msg,
			     KMsgBox::STOP, i18n("OK"));
#else
	    KMessageBox::sorry(parentWidget(), msg);
#endif
	    m_executable = "";
	    m_corefile = "";		/* don't process core file */
	    m_haveExecutable = false;
	}
	break;
    case DCcorefile:
	// in any event we have an executable at this point
	m_haveExecutable = true;
	if (m_d->parseCoreFile(output)) {
	    // loading a core is like stopping at a breakpoint
	    m_programActive = true;
	    handleRunCommands(output);
	    // do not reset m_corefile
	} else {
	    // report error
	    QString msg = m_d->driverName() + ": " + QString(output);
#if QT_VERSION < 200
	    KMsgBox::message(parentWidget(), kapp->appName(), msg,
			     KMsgBox::EXCLAMATION, i18n("OK"));
#else
	    KMessageBox::sorry(parentWidget(), msg);
#endif
	    // if core file was loaded from command line, revert to info line main
	    if (!cmd->m_byUser) {
		m_d->queueCmd(DCinfolinemain, DebuggerDriver::QMnormal);
	    }
	    m_corefile = QString();	/* core file not available any more */
	}
	break;
    case DCinfolinemain:
	// ignore the output, marked file info follows
	m_haveExecutable = true;
	break;
    case DCinfolocals:
	// parse local variables
	if (output[0] != '\0') {
	    handleLocals(output);
	}
	break;
    case DCinforegisters:
	handleRegisters(output);
	break;
    case DCframe:
	handleFrameChange(output);
	updateAllExprs();
	break;
    case DCbt:
	handleBacktrace(output);
	updateAllExprs();
	break;
    case DCprint:
	handlePrint(cmd, output);
	break;
    case DCattach:
    case DCrun:
    case DCcont:
    case DCstep:
    case DCnext:
    case DCfinish:
    case DCuntil:
	handleRunCommands(output);
	break;
    case DCkill:
	m_programRunning = m_programActive = false;
	// erase PC
	emit updatePC(QString(), -1, 0);
	break;
    case DCbreaktext:
    case DCbreakline:
    case DCtbreakline:
	newBreakpoint(output);
	// fall through
    case DCdelete:
    case DCenable:
    case DCdisable:
	// these commands need immediate response
	m_d->queueCmd(DCinfobreak, DebuggerDriver::QMoverrideMoreEqual);
	break;
    case DCinfobreak:
	// note: this handler must not enqueue a command, since
	// DCinfobreak is used at various different places.
	updateBreakList(output);
	emit lineItemsChanged();
	break;
    case DCfindType:
	handleFindType(cmd, output);
	break;
    case DCprintStruct:
    case DCprintQStringStruct:
	handlePrintStruct(cmd, output);
	break;
    case DCinfosharedlib:
	handleSharedLibs(output);
	break;
    case DCcondition:
    case DCignore:
	// we are not interested in the output
	break;
    }
}

void KDebugger::backgroundUpdate()
{
    /*
     * If there are still expressions that need to be updated, then do so.
     */
    if (m_programActive)
	evalExpressions();
}

void KDebugger::handleRunCommands(const char* output)
{
    uint flags = m_d->parseProgramStopped(output, m_statusMessage);
    emit updateStatusMessage();

    m_programActive = flags & DebuggerDriver::SFprogramActive;

    // refresh files if necessary
    if (flags & DebuggerDriver::SFrefreshSource) {
	TRACE("re-reading files");
	emit executableUpdated();
    }

    /*
     * If we stopped at a breakpoint, we must update the breakpoint list
     * because the hit count changes. Also, if the breakpoint was temporary
     * it would go away now.
     */
    if ((flags & (DebuggerDriver::SFrefreshBreak|DebuggerDriver::SFrefreshSource)) ||
	haveTemporaryBP())
    {
	m_d->queueCmd(DCinfobreak, DebuggerDriver::QMoverride);
    }

    /*
     * If we haven't listed the shared libraries yet, do so. We must do
     * this before we emit any commands that list variables, since the type
     * libraries depend on the shared libraries.
     */
    if (!m_sharedLibsListed) {
	// must be a high-priority command!
	m_d->executeCmd(DCinfosharedlib);
    }

    // get the backtrace if the program is running
    if (m_programActive) {
	m_d->queueCmd(DCbt, DebuggerDriver::QMoverride);
    } else {
	// program finished: erase PC
	emit updatePC(QString(), -1, 0);
	// dequeue any commands in the queues
	m_d->flushCommands();
    }

    m_programRunning = false;
}

void KDebugger::slotInferiorRunning()
{
    m_programRunning = true;
}

void KDebugger::updateAllExprs()
{
    if (!m_programActive)
	return;

    // retrieve local variables
    m_d->queueCmd(DCinfolocals, DebuggerDriver::QMoverride);

    // retrieve registers
    m_d->queueCmd(DCinforegisters, DebuggerDriver::QMoverride);

    // update watch expressions
    KTreeViewItem* item = m_watchVariables.itemAt(0);
    for (; item != 0; item = item->getSibling()) {
	m_watchEvalExpr.append(static_cast<VarTree*>(item));
    }
}

void KDebugger::updateProgEnvironment(const QString& args, const QString& wd,
				      const QDict<EnvVar>& newVars)
{
    m_programArgs = args;
    m_d->executeCmd(DCsetargs, m_programArgs);
    TRACE("new pgm args: " + m_programArgs + "\n");

    m_programWD = wd.stripWhiteSpace();
    if (!m_programWD.isEmpty()) {
	m_d->executeCmd(DCcd, m_programWD);
	TRACE("new wd: " + m_programWD + "\n");
    }

    QDictIterator<EnvVar> it = newVars;
    EnvVar* val;
    for (; (val = it) != 0; ++it) {
	switch (val->status) {
	case EnvVar::EVnew:
	    m_envVars.insert(it.currentKey(), val);
	    // fall thru
	case EnvVar::EVdirty:
	    // the value must be in our list
	    ASSERT(m_envVars[it.currentKey()] == val);
	    // update value
	    m_d->executeCmd(DCsetenv, it.currentKey(), val->value);
	    break;
	case EnvVar::EVdeleted:
	    // must be in our list
	    ASSERT(m_envVars[it.currentKey()] == val);
	    // delete value
	    m_d->executeCmd(DCsetenv, it.currentKey());
	    m_envVars.remove(it.currentKey());
	    break;
	default:
	    ASSERT(false);
	case EnvVar::EVclean:
	    // variable not changed
	    break;
	}
    }
}

void KDebugger::handleLocals(const char* output)
{
    // retrieve old list of local variables
    QStrList oldVars;
    m_localVariables.exprList(oldVars);

    /*
     *  Get local variables.
     */
    QList<VarTree> newVars;
    parseLocals(output, newVars);

    /*
     * Clear any old VarTree item pointers, so that later we don't access
     * dangling pointers.
     */
    m_localVariables.clearPendingUpdates();

    // reduce flicker
    bool autoU = m_localVariables.autoUpdate();
    m_localVariables.setAutoUpdate(false);
    bool repaintNeeded = false;

    /*
     * Match old variables against new ones.
     */
    for (const char* n = oldVars.first(); n != 0; n = oldVars.next()) {
	// lookup this variable in the list of new variables
	VarTree* v = newVars.first();
	while (v != 0 && strcmp(v->getText(), n) != 0) {
	    v = newVars.next();
	}
	if (v == 0) {
	    // old variable not in the new variables
	    TRACE(QString("old var deleted: ") + n);
	    v = m_localVariables.topLevelExprByName(n);
	    removeExpr(&m_localVariables, v);
	    if (v != 0) repaintNeeded = true;
	} else {
	    // variable in both old and new lists: update
	    TRACE(QString("update var: ") + n);
	    m_localVariables.updateExpr(newVars.current());
	    // remove the new variable from the list
	    newVars.remove();
	    delete v;
#if QT_VERSION >= 200
	    repaintNeeded = true;
#endif
	}
    }
    // insert all remaining new variables
    for (VarTree* v = newVars.first(); v != 0; v = newVars.next()) {
	TRACE("new var: " + v->getText());
	m_localVariables.insertExpr(v);
	repaintNeeded = true;
    }

    // repaint
    m_localVariables.setAutoUpdate(autoU);
    if (repaintNeeded && autoU && m_localVariables.isVisible())
	m_localVariables.repaint();
}

void KDebugger::parseLocals(const char* output, QList<VarTree>& newVars)
{
    QList<VarTree> vars;
    m_d->parseLocals(output, vars);

    QString origName;			/* used in renaming variables */
    while (vars.count() > 0)
    {
	VarTree* variable = vars.take(0);
	// get some types
	variable->inferTypesOfChildren(*m_typeTable);
	/*
	 * When gdb prints local variables, those from the innermost block
	 * come first. We run through the list of already parsed variables
	 * to find duplicates (ie. variables that hide local variables from
	 * a surrounding block). We keep the name of the inner variable, but
	 * rename those from the outer block so that, when the value is
	 * updated in the window, the value of the variable that is
	 * _visible_ changes the color!
	 */
	int block = 0;
	origName = variable->getText();
	for (VarTree* v = newVars.first(); v != 0; v = newVars.next()) {
	    if (variable->getText() == v->getText()) {
		// we found a duplicate, change name
		block++;
		QString newName = origName + " (" + QString().setNum(block) + ")";
		variable->setText(newName);
	    }
	}
	newVars.append(variable);
    }
}

bool KDebugger::handlePrint(CmdQueueItem* cmd, const char* output)
{
    ASSERT(cmd->m_expr != 0);

    VarTree* variable = parseExpr(output, true);
    if (variable == 0)
	return false;

    // set expression "name"
    variable->setText(cmd->m_expr->getText());

    if (cmd->m_expr->m_varKind == VarTree::VKpointer) {
	/*
	 * We must insert a dummy parent, because otherwise variable's value
	 * would overwrite cmd->m_expr's value.
	 */
	VarTree* dummyParent = new VarTree(variable->getText(), VarTree::NKplain);
	dummyParent->m_varKind = VarTree::VKdummy;
	// the name of the parsed variable is the address of the pointer
	QString addr = "*" + cmd->m_expr->m_value;
	variable->setText(addr);
	variable->m_nameKind = VarTree::NKaddress;

	dummyParent->appendChild(variable);
	dummyParent->setDeleteChildren(true);
	TRACE("update ptr: " + cmd->m_expr->getText());
	cmd->m_exprWnd->updateExpr(cmd->m_expr, dummyParent);
	delete dummyParent;
    } else {
	TRACE("update expr: " + cmd->m_expr->getText());
	cmd->m_exprWnd->updateExpr(cmd->m_expr, variable);
	delete variable;
    }

    evalExpressions();			/* enqueue dereferenced pointers */

    return true;
}

VarTree* KDebugger::parseExpr(const char* output, bool wantErrorValue)
{
    VarTree* variable;

    // check for error conditions
    bool goodValue = m_d->parsePrintExpr(output, wantErrorValue, variable);

    if (variable != 0 && goodValue)
    {
	// get some types
	variable->inferTypesOfChildren(*m_typeTable);
    }
    return variable;
}

// parse the output of bt
void KDebugger::handleBacktrace(const char* output)
{
    // reduce flicker
    m_btWindow.setAutoUpdate(false);

    m_btWindow.clear();

    QList<StackFrame> stack;
    m_d->parseBackTrace(output, stack);

    if (stack.count() > 0) {
	StackFrame* frm = stack.take(0);
	// first frame must set PC
	// note: frm->lineNo is zero-based
	emit updatePC(frm->fileName, frm->lineNo, frm->frameNo);

	do {
	    QString func;
	    if (frm->var != 0)
		func = frm->var->getText();
	    else
		func = frm->fileName + ":" + QString().setNum(frm->lineNo+1);
	    m_btWindow.insertItem(func);
	    TRACE("frame " + func + " (" + frm->fileName + ":" +
		  QString().setNum(frm->lineNo+1) + ")");
	    delete frm;
	}
	while ((frm = stack.take()) != 0);
    }

    m_btWindow.setAutoUpdate(true);
    m_btWindow.repaint();
}

void KDebugger::gotoFrame(int frame)
{
    m_d->executeCmd(DCframe, frame);
}

void KDebugger::handleFrameChange(const char* output)
{
    QString fileName;
    int frameNo;
    int lineNo;
    if (m_d->parseFrameChange(output, frameNo, fileName, lineNo)) {
	/* lineNo can be negative here if we can't find a file name */
	emit updatePC(fileName, lineNo, frameNo);
    } else {
	emit updatePC(fileName, -1, frameNo);
    }
}

void KDebugger::evalExpressions()
{
    // evaluate expressions in the following order:
    //   watch expressions
    //   pointers in local variables
    //   pointers in watch expressions
    //   types in local variables
    //   types in watch expressions
    //   pointers in 'this'
    //   types in 'this'
    
    VarTree* exprItem = m_watchEvalExpr.first();
    if (exprItem != 0) {
	m_watchEvalExpr.remove();
	QString expr = exprItem->computeExpr();
	TRACE("watch expr: " + expr);
	CmdQueueItem* cmd = m_d->queueCmd(DCprint, expr, DebuggerDriver::QMoverride);
	// remember which expr this was
	cmd->m_expr = exprItem;
	cmd->m_exprWnd = &m_watchVariables;
    } else {
	ExprWnd* wnd;
	VarTree* exprItem;
#define POINTER(widget) \
		wnd = &widget; \
		exprItem = widget.nextUpdatePtr(); \
		if (exprItem != 0) goto pointer
#define STRUCT(widget) \
		wnd = &widget; \
		exprItem = widget.nextUpdateStruct(); \
		if (exprItem != 0) goto ustruct
#define TYPE(widget) \
		wnd = &widget; \
		exprItem = widget.nextUpdateType(); \
		if (exprItem != 0) goto type
    repeat:
	POINTER(m_localVariables);
	POINTER(m_watchVariables);
	STRUCT(m_localVariables);
	STRUCT(m_watchVariables);
	TYPE(m_localVariables);
	TYPE(m_watchVariables);
#undef POINTER
#undef STRUCT
#undef TYPE
	return;

	pointer:
	// we have an expression to send
	dereferencePointer(wnd, exprItem, false);
	return;

	ustruct:
	// paranoia
	if (exprItem->m_type == 0 || exprItem->m_type == TypeInfo::unknownType())
	    goto repeat;
	evalInitialStructExpression(exprItem, wnd, false);
	return;

	type:
	/*
	 * Sometimes a VarTree gets registered twice for a type update. So
	 * it may happen that it has already been updated. Hence, we ignore
	 * it here and go on to the next task.
	 */
	if (exprItem->m_type != 0)
	    goto repeat;
	determineType(wnd, exprItem);
    }
}

void KDebugger::dereferencePointer(ExprWnd* wnd, VarTree* exprItem,
				   bool immediate)
{
    ASSERT(exprItem->m_varKind == VarTree::VKpointer);

    QString expr = exprItem->computeExpr();
    TRACE("dereferencing pointer: " + expr);
    QString queueExpr = "*(" + expr + ")";
    CmdQueueItem* cmd;
    if (immediate) {
	cmd = m_d->queueCmd(DCprint, queueExpr, DebuggerDriver::QMoverrideMoreEqual);
    } else {
	cmd = m_d->queueCmd(DCprint, queueExpr, DebuggerDriver::QMoverride);
    }
    // remember which expr this was
    cmd->m_expr = exprItem;
    cmd->m_exprWnd = wnd;
}

void KDebugger::determineType(ExprWnd* wnd, VarTree* exprItem)
{
    ASSERT(exprItem->m_varKind == VarTree::VKstruct);

    QString expr = exprItem->computeExpr();
    TRACE("get type of: " + expr);
    CmdQueueItem* cmd;
    cmd = m_d->queueCmd(DCfindType, expr, DebuggerDriver::QMoverride);

    // remember which expr this was
    cmd->m_expr = exprItem;
    cmd->m_exprWnd = wnd;
}

void KDebugger::handleFindType(CmdQueueItem* cmd, const char* output)
{
    QString type;
    if (m_d->parseFindType(output, type))
    {
	ASSERT(cmd != 0 && cmd->m_expr != 0);

	TypeInfo* info = m_typeTable->lookup(type);

	if (info == 0) {
	    /*
	     * We've asked gdb for the type of the expression in
	     * cmd->m_expr, but it returned a name we don't know. The base
	     * class (and member) types have been checked already (at the
	     * time when we parsed that particular expression). Now it's
	     * time to derive the type from the base classes as a last
	     * resort.
	     */
	    info = cmd->m_expr->inferTypeFromBaseClass();
	    // if we found a type through this method, register an alias
	    if (info != 0) {
		TRACE("infered alias: " + type);
		m_typeTable->registerAlias(type, info);
	    }
	}
	if (info == 0) {
	    TRACE("unknown type");
	    cmd->m_expr->m_type = TypeInfo::unknownType();
	} else {
	    cmd->m_expr->m_type = info;
	    /* since this node has a new type, we get its value immediately */
	    evalInitialStructExpression(cmd->m_expr, cmd->m_exprWnd, false);
	    return;
	}
    }

    evalExpressions();			/* queue more of them */
}

void KDebugger::handlePrintStruct(CmdQueueItem* cmd, const char* output)
{
    VarTree* var = cmd->m_expr;
    ASSERT(var != 0);
    ASSERT(var->m_varKind == VarTree::VKstruct);

    VarTree* partExpr;
    if (cmd->m_cmd != DCprintQStringStruct) {
	partExpr = parseExpr(output, false);
    } else {
	partExpr = m_d->parseQCharArray(output, false);
    }
    bool errorValue =
	partExpr == 0 ||
	/* we only allow simple values at the moment */
	partExpr->childCount() != 0;

    QString partValue;
    if (errorValue)
    {
	partValue = "???";
    } else {
	partValue = partExpr->m_value;
    }
    delete partExpr;
    partExpr = 0;

    /*
     * Updating a struct value works like this: var->m_partialValue holds
     * the value that we have gathered so far (it's been initialized with
     * var->m_type->m_displayString[0] earlier). Each time we arrive here,
     * we append the printed result followed by the next
     * var->m_type->m_displayString to var->m_partialValue.
     * 
     * If the expression we just evaluated was a guard expression, and it
     * resulted in an error, we must not evaluate the real expression, but
     * go on to the next index. (We must still add the ??? to the value).
     * 
     * Next, if this was the length expression, we still have not seen the
     * real expression, but the length of a QString.
     */
    ASSERT(var->m_exprIndex >= 0 && var->m_exprIndex <= typeInfoMaxExpr);

    if (errorValue || !var->m_exprIndexUseGuard)
    {
	// add current partValue (which might be ???)
#if QT_VERSION < 200
	var->m_partialValue.detach();
#endif
	var->m_partialValue += partValue;
	var->m_exprIndex++;		/* next part */
	var->m_exprIndexUseGuard = true;
	var->m_partialValue += var->m_type->m_displayString[var->m_exprIndex];
    }
    else
    {
	// this was a guard expression that succeeded
	// go for the real expression
	var->m_exprIndexUseGuard = false;
    }

    /* go for more sub-expressions if needed */
    if (var->m_exprIndex < var->m_type->m_numExprs) {
	/* queue a new print command with quite high priority */
	evalStructExpression(var, cmd->m_exprWnd, true);
	return;
    }

    cmd->m_exprWnd->updateStructValue(var);

    evalExpressions();			/* enqueue dereferenced pointers */
}

/* queues the first printStruct command for a struct */
void KDebugger::evalInitialStructExpression(VarTree* var, ExprWnd* wnd, bool immediate)
{
    var->m_exprIndex = 0;
    var->m_exprIndexUseGuard = true;
    var->m_partialValue = var->m_type->m_displayString[0];
    evalStructExpression(var, wnd, immediate);
}

/* queues a printStruct command; var must have been initialized correctly */
void KDebugger::evalStructExpression(VarTree* var, ExprWnd* wnd, bool immediate)
{
    QString base = var->computeExpr();
    QString exprFmt;
    if (var->m_exprIndexUseGuard) {
	exprFmt = var->m_type->m_guardStrings[var->m_exprIndex];
	if (exprFmt.isEmpty()) {
	    // no guard, omit it and go to expression
	    var->m_exprIndexUseGuard = false;
	}
    }
    if (!var->m_exprIndexUseGuard) {
	exprFmt = var->m_type->m_exprStrings[var->m_exprIndex];
    }

    SIZED_QString(expr, exprFmt.length() + base.length() + 10);
    expr.sprintf(exprFmt, base.data());

    DbgCommand dbgCmd = DCprintStruct;
    // check if this is a QString::Data
    if (strncmp(expr, "/QString::Data ", 15) == 0)
    {
	if (m_typeTable->parseQt2QStrings())
	{
	    expr = expr.mid(15, expr.length());	/* strip off /QString::Data */
	    dbgCmd = DCprintQStringStruct;
	} else {
	    /*
	     * This should not happen: the type libraries should be set up
	     * in a way that this can't happen. If this happens
	     * nevertheless it means that, eg., kdecore was loaded but qt2
	     * was not (only qt2 enables the QString feature).
	     */
	    // TODO: remove this "print"; queue the next printStruct instead
	    expr = "*0";
	}
    } else {
	expr = expr;
    }
    TRACE("evalStruct: " + expr + (var->m_exprIndexUseGuard ? " // guard" : " // real"));
    CmdQueueItem* cmd = m_d->queueCmd(dbgCmd, expr,
				      immediate  ?  DebuggerDriver::QMoverrideMoreEqual
				      : DebuggerDriver::QMnormal);

    // remember which expression this was
    cmd->m_expr = var;
    cmd->m_exprWnd = wnd;
}

/* removes expression from window */
void KDebugger::removeExpr(ExprWnd* wnd, VarTree* var)
{
    if (var == 0)
	return;

    // must remove any references to var from command queues
    m_d->dequeueCmdByVar(var);

    wnd->removeExpr(var);
}

void KDebugger::handleSharedLibs(const char* output)
{
    // delete all known libraries
    m_sharedLibs.clear();

    // parse the table of shared libraries
    m_d->parseSharedLibs(output, m_sharedLibs);
    m_sharedLibsListed = true;

    // get type libraries
    m_typeTable->loadLibTypes(m_sharedLibs);
}

CmdQueueItem* KDebugger::loadCoreFile()
{
    return m_d->queueCmd(DCcorefile, m_corefile, DebuggerDriver::QMoverride);
}

void KDebugger::slotLocalsExpanding(KTreeViewItem* item, bool& allow)
{
    exprExpandingHelper(&m_localVariables, item, allow);
}

void KDebugger::slotWatchExpanding(KTreeViewItem* item, bool& allow)
{
    exprExpandingHelper(&m_watchVariables, item, allow);
}

void KDebugger::exprExpandingHelper(ExprWnd* wnd, KTreeViewItem* item, bool&)
{
    VarTree* exprItem = static_cast<VarTree*>(item);
    if (exprItem->m_varKind != VarTree::VKpointer) {
	return;
    }
    dereferencePointer(wnd, exprItem, true);
}

// add the expression in the edit field to the watch expressions
void KDebugger::addWatch(const QString& t)
{
    QString expr = t.stripWhiteSpace();
    if (expr.isEmpty())
	return;
    VarTree* exprItem = new VarTree(expr, VarTree::NKplain);
    m_watchVariables.insertExpr(exprItem);

    // if we are boring ourselves, send down the command
    if (m_programActive) {
	m_watchEvalExpr.append(exprItem);
	if (m_d->isIdle()) {
	    evalExpressions();
	}
    }
}

// delete a toplevel watch expression
void KDebugger::slotDeleteWatch()
{
    // delete only allowed while debugger is idle; or else we might delete
    // the very expression the debugger is currently working on...
    if (!m_d->isIdle())
	return;

    int index = m_watchVariables.currentItem();
    if (index < 0)
	return;
    
    VarTree* item = static_cast<VarTree*>(m_watchVariables.itemAt(index));
    if (!item->isToplevelExpr())
	return;

    // remove the variable from the list to evaluate
    if (m_watchEvalExpr.findRef(item) >= 0) {
	m_watchEvalExpr.remove();
    }
    removeExpr(&m_watchVariables, item);
    // item is invalid at this point!
}

void KDebugger::startAnimation(bool fast)
{
    int interval = fast ? 50 : 150;
    if (!m_animationTimer.isActive()) {
	m_animationTimer.start(interval);
    } else if (m_animationInterval != interval) {
	m_animationTimer.changeInterval(interval);
    }
    m_animationInterval = interval;
}

void KDebugger::stopAnimation()
{
    if (m_animationTimer.isActive()) {
	m_animationTimer.stop();
	m_animationInterval = 0;
    }
}

void KDebugger::slotUpdateAnimation()
{
    if (m_d->isIdle()) {
	stopAnimation();
    } else {
	/*
	 * Slow animation while program is stopped (i.e. while variables
	 * are displayed)
	 */
	bool slow = isReady() && m_programActive && !m_programRunning;
	startAnimation(!slow);
    }
}

void KDebugger::handleRegisters(const char* output)
{
    QList<RegisterInfo> regs;
    m_d->parseRegisters(output, regs);

    emit registersChanged(regs);

    // delete them all
    regs.setAutoDelete(true);
}

/*
 * The output of the DCbreak* commands has more accurate information about
 * the file and the line number.
 */
void KDebugger::newBreakpoint(const char* output)
{
    int id;
    QString file;
    int lineNo;
    if (!m_d->parseBreakpoint(output, id, file, lineNo))
	return;

    // see if it is new
    for (int i = m_brkpts.size()-1; i >= 0; i--) {
	if (m_brkpts[i]->id == id) {
	    // not new; update
	    m_brkpts[i]->fileName = file;
	    m_brkpts[i]->lineNo = lineNo;
	    return;
	}
    }
    // yes, new
    Breakpoint* bp = new Breakpoint;
    bp->id = id;
    bp->temporary = false;
    bp->enabled = true;
    bp->hitCount = 0;
    bp->ignoreCount = 0;
    bp->fileName = file;
    bp->lineNo = lineNo;
    int n = m_brkpts.size();
    m_brkpts.resize(n+1);
    m_brkpts[n] = bp;
}

void KDebugger::updateBreakList(const char* output)
{
    // get the new list
    QList<Breakpoint> brks;
    brks.setAutoDelete(false);
    m_d->parseBreakList(output, brks);

    // merge new information into existing breakpoints

    QArray<Breakpoint*> oldbrks = m_brkpts;

    // move parsed breakpoints into m_brkpts
    m_brkpts.detach();
    m_brkpts.resize(brks.count());
    int n = 0;
    for (Breakpoint* bp = brks.first(); bp != 0; bp = brks.next())
    {
	m_brkpts[n++] = bp;
    }

    // go through all old breakpoints
    for (int i = oldbrks.size()-1; i >= 0; i--) {
	// is this one still alive?
	for (int j = m_brkpts.size()-1; j >= 0; j--)
	{
	    if (m_brkpts[j]->id == oldbrks[i]->id) {
		// yes, it is
		// keep accurate location
		m_brkpts[j]->fileName = oldbrks[i]->fileName;
		m_brkpts[j]->lineNo = oldbrks[i]->lineNo;
		break;
	    }
	}
    }

    // delete old breakpoints
    for (int i = oldbrks.size()-1; i >= 0; i--) {
	delete oldbrks[i];
    }

    emit breakpointsChanged();
}

// look if there is at least one temporary breakpoint
bool KDebugger::haveTemporaryBP() const
{
    for (int i = m_brkpts.size()-1; i >= 0; i--) {
	if (m_brkpts[i]->temporary)
	    return true;
    }
    return false;
}

Breakpoint* KDebugger::breakpointByFilePos(QString file, int lineNo)
{
    // look for exact file name match
    int i;
    for (i = m_brkpts.size()-1; i >= 0; i--) {
	if (m_brkpts[i]->lineNo == lineNo &&
	    m_brkpts[i]->fileName == file)
	{
	    return m_brkpts[i];
	}
    }
    // not found, so try basename
    // strip off directory part of file name
    int offset = file.findRev("/");
    if (offset < 0) {
	// that was already the basename, no need to scan the list again
	return 0;
    }
#if QT_VERSION < 200
    file.detach();
#endif
    file.remove(0, offset+1);

    for (i = m_brkpts.size()-1; i >= 0; i--) {
	if (m_brkpts[i]->lineNo == lineNo &&
	    m_brkpts[i]->fileName == file)
	{
	    return m_brkpts[i];
	}
    }

    // not found
    return 0;
}

void KDebugger::slotValuePopup(const QString& expr)
{
    // search the local variables for a match
    VarTree* v = m_localVariables.topLevelExprByName(expr);
    if (v == 0) {
	// not found, check watch expressions
	v = m_watchVariables.topLevelExprByName(expr);
	if (v == 0) {
	    // nothing found; do nothing
	    return;
	}
    }

    // construct the tip
    QString tip = v->getText() + " = ";
    if (!v->m_value.isEmpty())
    {
	tip += v->m_value;
    }
    else
    {
	// no value: we use some hint
	switch (v->m_varKind) {
	case VarTree::VKstruct:
	    tip += "{...}";
	    break;
	case VarTree::VKarray:
	    tip += "[...]";
	    break;
	default:
	    tip += "???";
	    break;
	}
    }
    emit valuePopup(tip);
}


#include "debugger.moc"
