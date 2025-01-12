/*
 * GuLinux Planetary Imager - https://github.com/GuLinux/PlanetaryImager
 * Copyright (C) 2016  Marco Gulino <marco@gulinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef SAVEFILEFORWARDER_H
#define SAVEFILEFORWARDER_H

#include "c++/dptr.h"
#include <QObject>
#include "commons/fwd.h"
#include "network/networkreceiver.h"

FWD_PTR(SaveImages)
FWD(Imager)
FWD_PTR(NetworkDispatcher)
FWD_PTR(SaveFileForwarder)

class SaveFileForwarder : public QObject, public NetworkReceiver
{
  Q_OBJECT
public:
  SaveFileForwarder(const SaveImagesPtr &save_images, const NetworkDispatcherPtr &dispatcher);
  ~SaveFileForwarder();
  void setImager(Imager *imager);
signals:
  void isRecording(bool recording);
private:
  DPTR
};

#endif // SAVEFILEFORWARDER_H
