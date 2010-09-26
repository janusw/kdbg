/*
 * Copyright Johannes Sixt
 * This file is licensed under the GNU General Public License Version 2.
 * See the file COPYING in the toplevel directory of the source directory.
 */

#include "watchwindow.h"
#include <klocale.h>			/* i18n */
#include <Q3DragObject>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>

WatchWindow::WatchWindow(QWidget* parent) :
	QWidget(parent),
	m_watchEdit(this),
	m_watchAdd(i18n(" Add "), this),
	m_watchDelete(i18n(" Del "), this),
	m_watchVariables(this, i18n("Expression")),
	m_watchV(this),
	m_watchH()
{
    // setup the layout
    m_watchAdd.setMinimumSize(m_watchAdd.sizeHint());
    m_watchDelete.setMinimumSize(m_watchDelete.sizeHint());
    m_watchV.setMargin(0);
    m_watchV.setSpacing(0);
    m_watchH.setMargin(0);
    m_watchH.setSpacing(0);
    m_watchV.addLayout(&m_watchH);
    m_watchV.addWidget(&m_watchVariables);
    m_watchH.addWidget(&m_watchEdit);
    m_watchH.addWidget(&m_watchAdd);
    m_watchH.addWidget(&m_watchDelete);

    connect(&m_watchEdit, SIGNAL(returnPressed()), SIGNAL(addWatch()));
    connect(&m_watchAdd, SIGNAL(clicked()), SIGNAL(addWatch()));
    connect(&m_watchDelete, SIGNAL(clicked()), SIGNAL(deleteWatch()));
    connect(&m_watchVariables, SIGNAL(currentChanged(Q3ListViewItem*)),
	    SLOT(slotWatchHighlighted()));

    m_watchVariables.installEventFilter(this);
    setAcceptDrops(true);
}

WatchWindow::~WatchWindow()
{
}

bool WatchWindow::eventFilter(QObject*, QEvent* ev)
{
    if (ev->type() == QEvent::KeyPress)
    {
	QKeyEvent* kev = static_cast<QKeyEvent*>(ev);
	if (kev->key() == Qt::Key_Delete) {
	    emit deleteWatch();
	    return true;
	}
    }
    return false;
}

void WatchWindow::dragEnterEvent(QDragEnterEvent* event)
{
    event->accept(Q3TextDrag::canDecode(event));
}

void WatchWindow::dropEvent(QDropEvent* event)
{
    QString text;
    if (Q3TextDrag::decode(event, text))
    {
	// pick only the first line
	text = text.stripWhiteSpace();
	int pos = text.find('\n');
	if (pos > 0)
	    text.truncate(pos);
	text = text.stripWhiteSpace();
	if (!text.isEmpty())
	    emit textDropped(text);
    }
}


// place the text of the hightlighted watch expr in the edit field
void WatchWindow::slotWatchHighlighted()
{
    VarTree* expr = m_watchVariables.selectedItem();
    QString text = expr ? expr->computeExpr() : QString();
    m_watchEdit.setText(text);
}

#include "watchwindow.moc"