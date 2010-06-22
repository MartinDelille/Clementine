/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MACSYSTEMTRAYICON_H
#define MACSYSTEMTRAYICON_H

#include "systemtrayicon.h"

#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>

class MacSystemTrayIconPrivate;

class MacSystemTrayIcon : public SystemTrayIcon, boost::noncopyable {
  Q_OBJECT

public:
  MacSystemTrayIcon(QObject* parent = 0);
  ~MacSystemTrayIcon();

  void SetupMenu(QAction* previous, QAction* play, QAction* stop,
                 QAction* stop_after, QAction* next, QAction* love,
                 QAction* ban, QAction* quit);
  bool IsVisible() const;
  void SetVisible(bool visible);

  void ShowPopup(const QString &summary, const QString &message, int timeout);

private:
  void SetupMenuItem(QAction* action);

private slots:
  void ActionChanged();

protected:
  // SystemTrayIcon
  void UpdateIcon();

private:
  QPixmap orange_icon_;
  QPixmap grey_icon_;

  boost::scoped_ptr<MacSystemTrayIconPrivate> p_;
};

#endif // MACSYSTEMTRAYICON_H
