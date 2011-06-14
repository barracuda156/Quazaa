/*
** widgetsecurity.h
**
** Copyright © Quazaa Development Team, 2009-2011.
** This file is part of QUAZAA (quazaa.sourceforge.net)
**
** Quazaa is free software; this file may be used under the terms of the GNU
** General Public License version 3.0 or later as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.
**
** Quazaa is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
**
** Please review the following information to ensure the GNU General Public
** License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** You should have received a copy of the GNU General Public License version
** 3.0 along with Quazaa; if not, write to the Free Software Foundation,
** Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef WIDGETSECURITY_H
#define WIDGETSECURITY_H

#include <QMainWindow>
#include <QAbstractItemModel>

class CSecurityTableModel;

namespace Ui
{
	class WidgetSecurity;
}

class WidgetSecurity : public QMainWindow
{
	Q_OBJECT
public:
	CSecurityTableModel* m_pSecurityList;

public:
	WidgetSecurity(QWidget* parent = 0);
	~WidgetSecurity();

	void		setModel(QAbstractItemModel* model);
	QWidget*	treeView();
	void		saveWidget();

protected:
	void changeEvent(QEvent* e);

private:
	Ui::WidgetSecurity* ui;

signals:
	void requestDataUpdate();

public slots:
	void update();

private slots:
	void on_actionSubscribeSecurityList_triggered();
	void on_actionSecurityAddRule_triggered();
	 
};

#endif // WIDGETSECURITY_H
