// $Id$

// Copyright by Johannes Sixt
// This file is under GPL, the GNU General Public Licence

#include "winstack.h"
#include "commandids.h"
#include "sourcewnd.h"
#include <qbrush.h>
#include <qfileinfo.h>
#include <qlistbox.h>
#include <kapp.h>
#if QT_VERSION >= 200
#include <klocale.h>			/* i18n */
#else
#include <ctype.h>
#endif
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "mydebug.h"



WinStack::WinStack(QWidget* parent, const char* name) :
	QWidget(parent, name),
	m_activeWindow(0),
	m_windowMenu(0),
	m_itemMore(0),
	m_pcLine(-1),
	m_valueTip(this),
	m_tipLocation(1,1,10,10)
{
    // Call menu implementation helper
    initMenu();

    connect(&m_findDlg.m_buttonForward,
	    SIGNAL(clicked()), SLOT(slotFindForward()));
    connect(&m_findDlg.m_buttonBackward,
	    SIGNAL(clicked()), SLOT(slotFindBackward()));

    // Check for right click event.
    connect(this, SIGNAL(clickedRight(const QPoint &)),
	    SLOT(slotWidgetRightClick(const QPoint &)));
}

WinStack::~WinStack()
{
}

// All menu initializations.
void WinStack::initMenu()
{
    // Init float popup menu.
    m_menuFloat.insertItem(i18n("&Open Source..."), ID_FILE_OPEN);
    m_menuFloat.insertSeparator();
    m_menuFloat.insertItem(i18n("Step &into"), ID_PROGRAM_STEP);
    m_menuFloat.insertItem(i18n("Step &over"), ID_PROGRAM_NEXT);
    m_menuFloat.insertItem(i18n("Step o&ut"), ID_PROGRAM_FINISH);
    m_menuFloat.insertItem(i18n("Run to &cursor"), ID_PROGRAM_UNTIL);
    m_menuFloat.insertSeparator();
    m_menuFloat.insertItem(i18n("Set/Clear &breakpoint"), ID_BRKPT_SET);

    // Init float file popup.
    m_menuFileFloat.insertItem(i18n("&Open Source..."), ID_FILE_OPEN);
    m_menuFileFloat.insertSeparator();
    m_menuFileFloat.insertItem(i18n("&Executable..."), ID_FILE_EXECUTABLE);
    m_menuFileFloat.insertItem(i18n("&Core dump..."), ID_FILE_COREFILE);
}

void WinStack::setWindowMenu(QPopupMenu* menu)
{
    m_windowMenu = menu;
    if (menu == 0) {
	return;
    }
    
    // find entry More...
    m_itemMore = menu->indexOf(ID_WINDOW_MORE);
    // must contain item More...
    ASSERT(m_itemMore >= 0);

    m_textMore = menu->text(ID_WINDOW_MORE);
    menu->removeItemAt(m_itemMore);
}

void WinStack::menuCallback(int item)
{
    TRACE("menu item=" + QString().setNum(item));
    // check for window
    if ((item & ~ID_WINDOW_INDEX_MASK) == ID_WINDOW_MORE) {
	selectWindow(item & ID_WINDOW_INDEX_MASK);
	return;
    }
    
    switch (item) {
    case ID_FILE_RELOAD:
	if (m_activeWindow != 0) {
	    TRACE("reloading one file");
	    m_activeWindow->reloadFile();
	}
	break;
    case ID_VIEW_FINDDLG:
	if (m_findDlg.isVisible()) {
	    m_findDlg.done(0);
	} else {
	    m_findDlg.show();
	}
    }
}

void WinStack::mousePressEvent(QMouseEvent* mouseEvent)
{
    // Check if right button was clicked.
    if (mouseEvent->button() == RightButton)
    {
	emit clickedRight(mouseEvent->pos());
    } else {
	QWidget::mousePressEvent(mouseEvent);
    }
}


void WinStack::reloadAllFiles()
{
    SourceWindow* fw;
    for (fw = m_fileList.first(); fw != 0; fw = m_fileList.next()) {
	fw->reloadFile();
    }
}

void WinStack::activate(const QString& fileName, int lineNo)
{
    QFileInfo fi(fileName);

    if (!fi.isFile()) {
	/*
	 * We didn't find that file. Now check if it is a relative path and
	 * try m_lastOpenDir as prefix.
	 */
	TRACE(fi.filePath() + (" not found, looking in " + m_lastOpenDir));
	if (!fi.isRelative() || m_lastOpenDir.isEmpty()) {
	    return;
	}
	fi.setFile(m_lastOpenDir + "/" + fi.filePath());
	if (!fi.isFile()) {
	    return;
	}
    }
    // if this is not an absolute path name, make it one
    activatePath(fi.absFilePath(), lineNo);
}

bool WinStack::activatePath(QString pathName, int lineNo)
{
    // check whether the file is already open
    SourceWindow* fw;
    for (fw = m_fileList.first(); fw != 0; fw = m_fileList.next()) {
	if (fw->fileName() == pathName) {
	    break;
	}
    }
    if (fw == 0) {
	// not found, load it
	fw = new SourceWindow(pathName, this, "fileWindow");
	m_fileList.insert(0, fw);
	connect(fw, SIGNAL(lineChanged()),SIGNAL(lineChanged()));
	connect(fw, SIGNAL(clickedLeft(const QString&, int)),
		SIGNAL(toggleBreak(const QString&,int)));
	connect(fw, SIGNAL(clickedMid(const QString&, int)),
		SIGNAL(enadisBreak(const QString&,int)));

	// Comunication when right button is clicked.
	connect(fw, SIGNAL(clickedRight(const QPoint &)),
		SLOT(slotFileWindowRightClick(const QPoint &)));

	// disassemble code
	connect(fw, SIGNAL(disassemble(const QString&, int)),
		SIGNAL(disassemble(const QString&, int)));

	changeWindowMenu();
	
	// slurp the file in
	fw->loadFile();
	
	// set PC if there is one
	emit newFileLoaded();
	if (m_pcLine >= 0) {
	    setPC(true, m_pcFile, m_pcLine, m_pcFrame);
	}
    }
    return activateWindow(fw, lineNo);
}

bool WinStack::activateWindow(SourceWindow* fw, int lineNo)
{
    int index = m_fileList.findRef(fw);
    ASSERT(index >= 0);
    if (index < 0) {
	return false;
    }
    /*
     * If the file is not in the list of those that would appear in the
     * window menu, move it to the first position.
     */
    if (index >= 9) {
	m_fileList.remove();
	m_fileList.insert(0, fw);
	changeWindowMenu();
    }

    // make the line visible
    if (lineNo >= 0) {
	fw->scrollTo(lineNo);
    }

    // first resize the window, then lift it to the top
    fw->setGeometry(0,0, width(),height());
    fw->raise();
    fw->show();

    // set the focus to the new active window
    QWidget* oldActive = m_activeWindow;
    fw->setFocusPolicy(QWidget::StrongFocus);
    m_activeWindow = fw;
    if (oldActive != 0 && oldActive != fw) {
	// disable focus on non-active windows
	oldActive->setFocusPolicy(QWidget::NoFocus);
    }
    fw->setFocus();

    emit fileChanged();

    return true;
}

bool WinStack::activeLine(QString& fileName, int& lineNo)
{
    if (m_activeWindow == 0) {
	return false;
    }
    
    fileName = m_activeWindow->fileName();
    int dummyCol;
    m_activeWindow->cursorPosition(&lineNo, &dummyCol);
    return true;
}

void WinStack::changeWindowMenu()
{
    if (m_windowMenu == 0) {
	return;
    }

    // delete window entries
    while ((m_windowMenu->idAt(m_itemMore) & ~ID_WINDOW_INDEX_MASK) == ID_WINDOW_MORE) {
	m_windowMenu->removeItemAt(m_itemMore);
    }

    // insert current windows
    QString text;
    int index = 1;
    SourceWindow* fw = 0;
    for (fw = m_fileList.first(); fw != 0 && index < 10; fw = m_fileList.next()) {
	text.sprintf("&%d ", index);
	text += fw->fileName();
	m_windowMenu->insertItem(text, ID_WINDOW_MORE+index, m_itemMore+index-1);
	index++;
    }
    if (fw != 0) {
	// there are still windows
	m_windowMenu->insertItem(m_textMore, ID_WINDOW_MORE, m_itemMore+9);
    }
}

void WinStack::updateLineItems(const KDebugger* dbg)
{
    SourceWindow* fw = 0;
    for (fw = m_fileList.first(); fw != 0; fw = m_fileList.next()) {
	fw->updateLineItems(dbg);
    }
}

void WinStack::updatePC(const QString& fileName, int lineNo, int frameNo)
{
    if (m_pcLine >= 0) {
	setPC(false, m_pcFile, m_pcLine, m_pcFrame);
    }
    m_pcFile = fileName;
    m_pcLine = lineNo;
    m_pcFrame = frameNo;
    if (lineNo >= 0) {
	setPC(true, fileName, lineNo, frameNo);
    }
}

void WinStack::setPC(bool set, const QString& fileName, int lineNo, int frameNo)
{
    TRACE((set ? "set PC: " : "clear PC: ") + fileName +
	  QString().sprintf(":%d#%d", lineNo, frameNo));
    // find file
    SourceWindow* fw = 0;
    for (fw = m_fileList.first(); fw != 0; fw = m_fileList.next()) {
	if (fw->fileNameMatches(fileName)) {
	    fw->setPC(set, lineNo, frameNo);
	    break;
	}
    }
}

void WinStack::resizeEvent(QResizeEvent*)
{
    ASSERT(m_activeWindow == 0 || m_fileList.findRef(m_activeWindow) >= 0);
    if (m_activeWindow != 0) {
	m_activeWindow->resize(width(), height());
    }
}

void WinStack::slotFindForward()
{
    if (m_activeWindow != 0)
	m_activeWindow->find(m_findDlg.searchText(), m_findDlg.caseSensitive(),
			     SourceWindow::findForward);
}

void WinStack::slotFindBackward()
{
    if (m_activeWindow != 0)
	m_activeWindow->find(m_findDlg.searchText(), m_findDlg.caseSensitive(),
			     SourceWindow::findBackward);
}

void WinStack::slotFileWindowRightClick(const QPoint & pos)
{
    if (m_menuFloat.isVisible())
    {
	m_menuFloat.hide();
    }
    else
    {
	m_menuFloat.popup(mapToGlobal(pos));
    }
}

void WinStack::slotWidgetRightClick(const QPoint & pos)
{
    if (m_menuFileFloat.isVisible())
    {
	m_menuFileFloat.hide();
    }
    else
    {
	m_menuFileFloat.popup(mapToGlobal(pos));
    }
}

void WinStack::maybeTip(const QPoint& p)
{
    if (m_activeWindow == 0)
	return;

    // get the word at the point
    QString word;
    QRect r;
    if (!m_activeWindow->wordAtPoint(p, word, r))
	return;

    // must be valid
    assert(!word.isEmpty());
    assert(r.isValid());

    // remember the location
    m_tipLocation = r;

    emit initiateValuePopup(word);
}

void WinStack::slotShowValueTip(const QString& tipText)
{
    m_valueTip.tip(m_tipLocation, tipText);
}

void WinStack::slotDisassembled(const QString& fileName, int lineNo,
				const QList<DisassembledCode>& disass)
{
    // lookup the file
    SourceWindow* fw;
    for (fw = m_fileList.first(); fw != 0; fw = m_fileList.next()) {
	if (fw->fileNameMatches(fileName)) {
	    break;
	}
    }
    if (fw == 0) {
	// not found: ignore
	return;
    }

    fw->disassembled(lineNo, disass);
}


ValueTip::ValueTip(WinStack* parent) :
	QToolTip(parent)
{
}

void ValueTip::maybeTip(const QPoint& p)
{
    WinStack* w = static_cast<WinStack*>(parentWidget());
    w->maybeTip(p);
}


class MoreWindowsDialog : public QDialog
{
public:
    MoreWindowsDialog(QWidget* parent);
    virtual ~MoreWindowsDialog();

    void insertString(const char* text) { m_list.insertItem(text); }
    void setListIndex(int i) { m_list.setCurrentItem(i); }
    int listIndex() const { return m_list.currentItem(); }

protected:
    QListBox m_list;
    QPushButton m_buttonOK;
    QPushButton m_buttonCancel;
    QVBoxLayout m_layout;
    QHBoxLayout m_buttons;
};

MoreWindowsDialog::MoreWindowsDialog(QWidget* parent) :
	QDialog(parent, "morewindows", true),
	m_list(this, "windows"),
	m_buttonOK(this, "show"),
	m_buttonCancel(this, "cancel"),
	m_layout(this, 8),
	m_buttons(4)
{
    QString title = kapp->getCaption();
    title += i18n(": Open Windows");
    setCaption(title);

    m_list.setMinimumSize(250, 100);
    connect(&m_list, SIGNAL(selected(int)), SLOT(accept()));

    m_buttonOK.setMinimumSize(100, 30);
    connect(&m_buttonOK, SIGNAL(clicked()), SLOT(accept()));
    m_buttonOK.setText(i18n("Show"));
    m_buttonOK.setDefault(true);

    m_buttonCancel.setMinimumSize(100, 30);
    connect(&m_buttonCancel, SIGNAL(clicked()), SLOT(reject()));
    m_buttonCancel.setText(i18n("Cancel"));

    m_layout.addWidget(&m_list, 10);
    m_layout.addLayout(&m_buttons);
    m_buttons.addStretch(10);
    m_buttons.addWidget(&m_buttonOK);
    m_buttons.addSpacing(40);
    m_buttons.addWidget(&m_buttonCancel);
    m_buttons.addStretch(10);

    m_layout.activate();

    m_list.setFocus();
    resize(320, 320);
}

MoreWindowsDialog::~MoreWindowsDialog()
{
}

void WinStack::selectWindow(int id)
{
    int index = 0;

    if (id == 0) {
	// more windows selected: show windows in a list
	MoreWindowsDialog dlg(this);
	int i = 0;
	for (SourceWindow* fw = m_fileList.first(); fw != 0; fw = m_fileList.next()) {
	    dlg.insertString(fw->fileName());
	    if (m_activeWindow == fw) {
		index = i;
	    }
	    i++;
	}
	dlg.setListIndex(index);
	if (dlg.exec() == QDialog::Rejected)
	    return;
	index = dlg.listIndex();
    } else {
	index = (id & ID_WINDOW_INDEX_MASK)-1;
    }

    SourceWindow* fw = m_fileList.first();
    for (; index > 0; index--) {
	fw = m_fileList.next();
    }
    ASSERT(fw != 0);
	
    activateWindow(fw, -1);
}


FindDialog::FindDialog() :
	QDialog(0, "find", false),
	m_searchText(this, "text"),
	m_caseCheck(this, "case"),
	m_buttonForward(this, "forward"),
	m_buttonBackward(this, "backward"),
	m_buttonClose(this, "close"),
	m_layout(this, 8),
	m_buttons(4)
{
    setCaption(QString(kapp->getCaption()) + i18n(": Search"));

    m_searchText.setMinimumSize(330, 24);
    m_searchText.setMaxLength(10000);
    m_searchText.setFrame(true);

    m_caseCheck.setText(i18n("&Case sensitive"));
    m_caseCheck.setChecked(true);
    m_buttonForward.setText(i18n("&Forward"));
    m_buttonForward.setDefault(true);
    m_buttonBackward.setText(i18n("&Backward"));
    m_buttonClose.setText(i18n("Close"));

    m_caseCheck.setMinimumSize(330, 24);

    // get maximum size of buttons
    QSize maxSize(80,30);
#if QT_VERSION >= 140
    maxSize.expandedTo(m_buttonForward.sizeHint());
    maxSize.expandedTo(m_buttonBackward.sizeHint());
    maxSize.expandedTo(m_buttonClose.sizeHint());
#endif

    m_buttonForward.setMinimumSize(maxSize);
    m_buttonBackward.setMinimumSize(maxSize);
    m_buttonClose.setMinimumSize(maxSize);

    connect(&m_buttonClose, SIGNAL(clicked()), SLOT(reject()));

    m_layout.addWidget(&m_searchText);
    m_layout.addWidget(&m_caseCheck);
    m_layout.addLayout(&m_buttons);
    m_layout.addStretch(10);
    m_buttons.addWidget(&m_buttonForward);
    m_buttons.addStretch(10);
    m_buttons.addWidget(&m_buttonBackward);
    m_buttons.addStretch(10);
    m_buttons.addWidget(&m_buttonClose);

    m_layout.activate();

    m_searchText.setFocus();
    resize( 350, 120 );
}

FindDialog::~FindDialog()
{
}

void FindDialog::closeEvent(QCloseEvent* ev)
{
    QDialog::closeEvent(ev);
    emit closed();
}

void FindDialog::done(int result)
{
    QDialog::done(result);
    emit closed();
}

#include "winstack.moc"
