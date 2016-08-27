/*
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

#include "saveimages.h"
#include <QFile>
#include <QThread>
#include <QDebug>
#include <functional>
#include "utils.h"

#include <QtConcurrent/QtConcurrent>
#include <functional>
#include <cstring>
#include "fps_counter.h"
#include "configuration.h"
#include <boost/lockfree/spsc_queue.hpp>
#include "opencv_utils.h"
#include <Qt/strings.h>
#include "output_writers/filewriter.h"


using namespace std;
using namespace std::placeholders;

struct RecordingInformation {
  RecordingInformation(Configuration &configuration, Imager *imager);
  ~RecordingInformation();
  void set_base_filename(const QString &filename);
  void set_ended(int total_frames, int width, int height);
  QVariantMap properties;
  typedef shared_ptr<RecordingInformation> ptr;
  QString filename;
  QDateTime started;
};

RecordingInformation::RecordingInformation(Configuration& configuration, Imager *imager)
{
  started = QDateTime::currentDateTime();
  properties["started"] = started.toString(Qt::ISODate);
  properties["camera"] = imager->name();
  properties["observer"] = configuration.observer();
  properties["telescope"] = configuration.telescope();
  QVariantMap camera_settings;
  for(auto setting: imager->controls()) {
    QVariantMap setting_value;
    setting_value["value"] = setting.value;
    static QMap<Imager::Control::Type, QString> types {
      {Imager::Control::Number, "number"},
      {Imager::Control::Combo, "combo"},
      {Imager::Control::Bool, "bool"}
    };
    setting_value["type"] = types[setting.type];
    if(setting.type == Imager::Control::Combo) {
      QVariantMap choices;
      for(auto choice: setting.choices)
	choices[choice.label] = choice.value;
      setting_value["choices"] = choices;
    }
    if(setting.type == Imager::Control::Number && setting.is_duration) {
      setting_value["type"] = "duration";
      QList<QPair<QString, double>> units {
	{"seconds", 1}, {"milliseconds", 1000}, {"microseconds", 1000000}
      };
      for(auto unit: units) {
	setting_value["value_%1"_q % unit.first] = setting.value * setting.duration_unit.count() * unit.second;
      }
    }
    camera_settings[setting.name] = setting_value;
  }
  properties["camera-settings"] = camera_settings;
}

void RecordingInformation::set_ended(int total_frames, int width, int height)
{
  auto ended =QDateTime::currentDateTime();
  auto elapsed = started.secsTo(ended);
  properties["ended"] = ended.toString(Qt::ISODate);
  properties["total-frames"] = total_frames;
  properties["width"] = width;
  properties["height"] = height;
  properties["mean-fps"] = static_cast<double>(total_frames) / static_cast<double>(elapsed);
}

void RecordingInformation::set_base_filename(const QString& filename)
{
  this->filename = filename + ".txt";
}

RecordingInformation::~RecordingInformation()
{
  QFile file(filename);
  if(filename.isEmpty() || ! file.open(QIODevice::WriteOnly))
    return;
  auto json = QJsonDocument::fromVariant(properties);
  file.write(json.toJson(QJsonDocument::Indented));
  file.close();
}



class WriterThreadWorker;
class SaveImages::Private {
public:
    Private(Configuration &configuration, SaveImages *q);
    Configuration &configuration;
    QThread recordingThread;
    FileWriter::Factory writerFactory();
    WriterThreadWorker *worker = nullptr;
    bool is_recording = false;
private:
    SaveImages *q;
};

SaveImages::Private::Private(Configuration &configuration, SaveImages* q) : configuration{configuration}, q{q}
{
}

SaveImages::SaveImages(Configuration& configuration, QObject* parent) : QObject(parent), dptr(configuration, this)
{
}


SaveImages::~SaveImages()
{
}


FileWriter::Factory SaveImages::Private::writerFactory()
{
  if(configuration.savefile().isEmpty()) {
    return {};
  }

  return FileWriter::factories()[configuration.save_format()];
}

class WriterThreadWorker : public QObject {
  Q_OBJECT
public:
  typedef function< FileWriter::Ptr() > FileWriterFactory;
  explicit WriterThreadWorker ( const FileWriterFactory& fileWriterFactory, uint64_t max_frames, long long int max_memory, bool& is_recording, SaveImages *saveImages, const RecordingInformation::ptr &recording_information, QObject* parent = 0 );
  virtual ~WriterThreadWorker();
public slots:
  virtual void handle(const cv::Mat& imageData);
  void run();
private:
  FileWriterFactory fileWriterFactory;
  shared_ptr<boost::lockfree::spsc_queue<cv::Mat>> framesQueue;
  uint64_t max_frames;
  long long max_memory;
  bool &is_recording;
  uint64_t dropped_frames = 0;
  SaveImages *saveImages;
  RecordingInformation::ptr recording_information;
};

WriterThreadWorker::WriterThreadWorker ( const WriterThreadWorker::FileWriterFactory& fileWriterFactory, uint64_t max_frames, long long int max_memory, bool& is_recording, SaveImages* saveImages, const RecordingInformation::ptr& recording_information, QObject* parent )
  : QObject(parent), fileWriterFactory(fileWriterFactory), max_frames(max_frames), max_memory{max_memory}, is_recording{is_recording}, saveImages{saveImages}, recording_information{recording_information}
{
}

WriterThreadWorker::~WriterThreadWorker()
{
}

#define MB(arg) (arg* 1024 * 1024)

void WriterThreadWorker::handle(const cv::Mat& imageData)
{
  auto imageDataSize = imageData.total()* imageData.elemSize();
  if(!framesQueue) {
    framesQueue = make_shared<boost::lockfree::spsc_queue<cv::Mat>>(max_memory/imageDataSize);
    qDebug() << "allocated framesqueue with " << max_memory << " bytes capacity (" << max_memory/imageDataSize << " frames)";
  }
  
  if(!framesQueue->push(imageData)) {
    qWarning() << "Frames queue too high, dropping frame";
    emit saveImages->droppedFrames(++dropped_frames);
  }
}


void WriterThreadWorker::run()
{
  {
    auto fileWriter = fileWriterFactory();
    if(recording_information)
      recording_information->set_base_filename(fileWriter->filename());
    fps_counter savefps{[=](double fps){ emit saveImages->saveFPS(fps);}, fps_counter::Elapsed};
    fps_counter meanfps{[=](double fps){ emit saveImages->meanFPS(fps);}, fps_counter::Elapsed, 1000, true};
    uint64_t frames = 0;
    emit saveImages->recording(fileWriter->filename());
    int width = -1, height = -1;
    while(is_recording && frames < max_frames) {
      cv::Mat frame;
      if(framesQueue && framesQueue->pop(frame)) {
	fileWriter->handle(frame);
	++savefps;
	++meanfps;
	emit saveImages->savedFrames(++frames);
	if(width == -1 || height == -1) {
	  width = frame.cols;
	  height = frame.rows;
	}
      } else {
	QThread::msleep(1);
      }
    }
    is_recording = false;
    if(recording_information)
      recording_information->set_ended(frames, width, height);
  }
  qDebug() << "closing thread";
  emit saveImages->finished();
  recording_information.reset();
  QThread::currentThread()->quit();
  qDebug() << "finished worker";
}


void SaveImages::handle(const cv::Mat& imageData)
{
  if(!d->is_recording)
    return;
  QtConcurrent::run(bind(&WriterThreadWorker::handle, d->worker, imageData));
}

void SaveImages::startRecording(Imager *imager)
{
  auto writerFactory = d->writerFactory();
  if(writerFactory) {
    RecordingInformation::ptr recording_information;
    if(d->configuration.save_info_file())
      recording_information = make_shared<RecordingInformation>(d->configuration, imager);
    d->worker = new WriterThreadWorker(bind(writerFactory, imager->name(), std::ref<Configuration>(d->configuration)),
				       d->configuration.recording_frames_limit() == 0 ? std::numeric_limits<long long>().max() : d->configuration.recording_frames_limit(), 
				       d->configuration.max_memory_usage(), 
				       d->is_recording, this, recording_information);

    connect(&d->recordingThread, &QThread::finished, bind(&SaveImages::finished, this));
    d->worker->moveToThread(&d->recordingThread);
    connect(&d->recordingThread, &QThread::started, d->worker, &WriterThreadWorker::run);
    connect(&d->recordingThread, &QThread::finished, d->worker, &WriterThreadWorker::deleteLater);
    d->is_recording = true;
    d->recordingThread.start();
  }
}



void SaveImages::endRecording()
{
  d->is_recording = false;
}


#include "saveimages.moc"
