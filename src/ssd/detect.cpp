// This is a demo code for using a SSD model to do detection.
// The code is modified from examples/cpp_classification/classification.cpp.
// Usage:
//    ssd_detect [FLAGS] model_file weights_file list_file
//
// where model_file is the .prototxt file defining the network architecture, and
// weights_file is the .caffemodel file containing the network parameters, and
// list_file contains a list of image files with the format as follows:
//    folder/img1.JPEG
//    folder/img2.JPEG
// list_file can also contain a list of video files with the format as follows:
//    folder/video1.mp4
//    folder/video2.mp4
//
#define USE_OPENCV

#include <gflags/gflags.h>
#include <caffe/caffe.hpp>
#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#endif  // USE_OPENCV
#include <algorithm>
#include <iomanip>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "helper/helper.h"

// GOTURN Tracker
#include "tracker/tracker.h"
#include "network/regressor_train.h"
#include "network/regressor.h"
#include "loader/loader_alov.h"
#include "loader/loader_vot.h"
#include "tracker/tracker.h"
#include "tracker/tracker_manager.h"

#include <chrono>
#include <thread>

#include "controller/dy_controller.h"

#ifdef USE_OPENCV
using namespace caffe;  // NOLINT(build/namespaces)
using namespace std;
using namespace cv;

#define DETECTION_TRACKING_DISAGREE_TH 0.7
#define SLEEP_MICROSECONDS 50.0
#define PERSON_LABEL 15
#define PERSON_GOOD_CONFIDENCE_TH 0.5
#define PERSON_EXIST_CONFIDENCE_TH 0.3
#define STOP_AREA_TH 0.6

// send robot command by dynamism 
float turnval = 0;
float speedval = 0;
int sitval = 0;
int standval = 0;
int walkval = 0;
DyController controller;

class Detector {
 public:
  Detector(const string& model_file,
           const string& weights_file,
           const string& mean_file,
           const string& mean_value);

  std::vector<vector<float> > Detect(const cv::Mat& img);

 private:
  void SetMean(const string& mean_file, const string& mean_value);

  void WrapInputLayer(std::vector<cv::Mat>* input_channels);

  void Preprocess(const cv::Mat& img,
                  std::vector<cv::Mat>* input_channels);

 private:
  boost::shared_ptr<Net<float> > net_;
  cv::Size input_geometry_;
  int num_channels_;
  cv::Mat mean_;
};

Detector::Detector(const string& model_file,
                   const string& weights_file,
                   const string& mean_file,
                   const string& mean_value) {
#ifdef CPU_ONLY
  Caffe::set_mode(Caffe::CPU);
#else
  Caffe::set_mode(Caffe::GPU);
#endif

  /* Load the network. */
  net_.reset(new Net<float>(model_file, TEST));
  net_->CopyTrainedLayersFrom(weights_file);

  CHECK_EQ(net_->num_inputs(), 1) << "Network should have exactly one input.";
  CHECK_EQ(net_->num_outputs(), 1) << "Network should have exactly one output.";

  Blob<float>* input_layer = net_->input_blobs()[0];
  num_channels_ = input_layer->channels();
  CHECK(num_channels_ == 3 || num_channels_ == 1)
    << "Input layer should have 1 or 3 channels.";
  input_geometry_ = cv::Size(input_layer->width(), input_layer->height());

  /* Load the binaryproto mean file. */
  SetMean(mean_file, mean_value);
}

std::vector<vector<float> > Detector::Detect(const cv::Mat& img) {
  Blob<float>* input_layer = net_->input_blobs()[0];
  input_layer->Reshape(1, num_channels_,
                       input_geometry_.height, input_geometry_.width);
  /* Forward dimension change to all layers. */
  net_->Reshape();

  std::vector<cv::Mat> input_channels;
  WrapInputLayer(&input_channels);

  Preprocess(img, &input_channels);

  net_->Forward();

  /* Copy the output layer to a std::vector */
  Blob<float>* result_blob = net_->output_blobs()[0];
  const float* result = result_blob->cpu_data();
  const int num_det = result_blob->height();
  vector<vector<float> > detections;
  for (int k = 0; k < num_det; ++k) {
    if (result[0] == -1) {
      // Skip invalid detection.
      result += 7;
      continue;
    }
    vector<float> detection(result, result + 7);
    detections.push_back(detection);
    result += 7;
  }
  return detections;
}

/* Load the mean file in binaryproto format. */
void Detector::SetMean(const string& mean_file, const string& mean_value) {
  cv::Scalar channel_mean;
  if (!mean_file.empty()) {
    CHECK(mean_value.empty()) <<
      "Cannot specify mean_file and mean_value at the same time";
    BlobProto blob_proto;
    ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);

    /* Convert from BlobProto to Blob<float> */
    Blob<float> mean_blob;
    mean_blob.FromProto(blob_proto);
    CHECK_EQ(mean_blob.channels(), num_channels_)
      << "Number of channels of mean file doesn't match input layer.";

    /* The format of the mean file is planar 32-bit float BGR or grayscale. */
    std::vector<cv::Mat> channels;
    float* data = mean_blob.mutable_cpu_data();
    for (int i = 0; i < num_channels_; ++i) {
      /* Extract an individual channel. */
      cv::Mat channel(mean_blob.height(), mean_blob.width(), CV_32FC1, data);
      channels.push_back(channel);
      data += mean_blob.height() * mean_blob.width();
    }

    /* Merge the separate channels into a single image. */
    cv::Mat mean;
    cv::merge(channels, mean);

    /* Compute the global mean pixel value and create a mean image
     * filled with this value. */
    channel_mean = cv::mean(mean);
    mean_ = cv::Mat(input_geometry_, mean.type(), channel_mean);
  }
  if (!mean_value.empty()) {
    CHECK(mean_file.empty()) <<
      "Cannot specify mean_file and mean_value at the same time";
    stringstream ss(mean_value);
    vector<float> values;
    string item;
    while (getline(ss, item, ',')) {
      float value = std::atof(item.c_str());
      values.push_back(value);
    }
    CHECK(values.size() == 1 || values.size() == num_channels_) <<
      "Specify either 1 mean_value or as many as channels: " << num_channels_;

    std::vector<cv::Mat> channels;
    for (int i = 0; i < num_channels_; ++i) {
      /* Extract an individual channel. */
      cv::Mat channel(input_geometry_.height, input_geometry_.width, CV_32FC1,
          cv::Scalar(values[i]));
      channels.push_back(channel);
    }
    cv::merge(channels, mean_);
  }
}

/* Wrap the input layer of the network in separate cv::Mat objects
 * (one per channel). This way we save one memcpy operation and we
 * don't need to rely on cudaMemcpy2D. The last preprocessing
 * operation will write the separate channels directly to the input
 * layer. */
void Detector::WrapInputLayer(std::vector<cv::Mat>* input_channels) {
  Blob<float>* input_layer = net_->input_blobs()[0];

  int width = input_layer->width();
  int height = input_layer->height();
  float* input_data = input_layer->mutable_cpu_data();
  for (int i = 0; i < input_layer->channels(); ++i) {
    cv::Mat channel(height, width, CV_32FC1, input_data);
    input_channels->push_back(channel);
    input_data += width * height;
  }
}

void Detector::Preprocess(const cv::Mat& img,
                            std::vector<cv::Mat>* input_channels) {
  /* Convert the input image to the input image format of the network. */
  cv::Mat sample;
  if (img.channels() == 3 && num_channels_ == 1)
    cv::cvtColor(img, sample, cv::COLOR_BGR2GRAY);
  else if (img.channels() == 4 && num_channels_ == 1)
    cv::cvtColor(img, sample, cv::COLOR_BGRA2GRAY);
  else if (img.channels() == 4 && num_channels_ == 3)
    cv::cvtColor(img, sample, cv::COLOR_BGRA2BGR);
  else if (img.channels() == 1 && num_channels_ == 3)
    cv::cvtColor(img, sample, cv::COLOR_GRAY2BGR);
  else
    sample = img;

  cv::Mat sample_resized;
  if (sample.size() != input_geometry_)
    cv::resize(sample, sample_resized, input_geometry_);
  else
    sample_resized = sample;

  cv::Mat sample_float;
  if (num_channels_ == 3)
    sample_resized.convertTo(sample_float, CV_32FC3);
  else
    sample_resized.convertTo(sample_float, CV_32FC1);

  cv::Mat sample_normalized;
  cv::subtract(sample_float, mean_, sample_normalized);

  /* This operation will write the separate BGR planes directly to the
   * input layer of the network because it is wrapped by the cv::Mat
   * objects in input_channels. */
  cv::split(sample_normalized, *input_channels);

  CHECK(reinterpret_cast<float*>(input_channels->at(0).data)
        == net_->input_blobs()[0]->cpu_data())
    << "Input channels are not wrapping the input layer of the network.";
}


// function check if the detection differ too much from the tracked result
bool DetectionTrackingDisagree(BoundingBox & tracked_bbox, BoundingBox & detection_bbox) {
  if (tracked_bbox.compute_IOU(detection_bbox) < DETECTION_TRACKING_DISAGREE_TH) {
    return true;
  }
  return false;
}

void DetectionTrackingProcessFrame(Mat & img, const int frame_count, Detector &detector, Regressor & regressor, Tracker &tracker, VideoWriter &video_writer, 
                                   const float confidence_threshold,  bool * tracker_initialised, bool save) {
  CHECK(!img.empty()) << "Error when read frame: " << frame_count;
  std::vector<vector<float> > detections = detector.Detect(img);
  // cout << "VideoCap, frame: " << frame_count << ", img.size(): " << img.size() << endl;

  // imshow("img after detection:", img);
  // waitKey(0);

  Mat img_track = img.clone();

  int closest_person_detection_id = -1;
  double max_region = -1;
  double best_person_confidence = -1;

  for (int i = 0; i < detections.size(); ++i) {
    const vector<float>& d = detections[i];
    // Detection format: [image_id, label, score, xmin, ymin, xmax, ymax].
    CHECK_EQ(d.size(), 7);
    const float score = d[2];
    if (score >= confidence_threshold) {

      double this_region = (d[5] * img.cols - d[3] * img.cols) * (d[6] * img.rows - d[4] * img.rows);

      // TODO: more clever way to filter out false positives
      if (static_cast<int>(d[1]) == PERSON_LABEL && this_region > max_region && score > PERSON_GOOD_CONFIDENCE_TH) {
        // cout << "this person detection score: " << score << endl;
        max_region = this_region;
        closest_person_detection_id = i;
      }

      // Update best person confidence
      if (static_cast<int>(d[1]) == PERSON_LABEL && score > best_person_confidence) {
        best_person_confidence = score;
      }
    }
  }

  const vector<float> &closest_person_detection = detections[closest_person_detection_id];
  
  cv::Mat img_visualise = img.clone();

  // for frame 0, initialise tracker, or when the detection and tracking disagree and at least valid closest_person_detection
  if (!(*tracker_initialised) && closest_person_detection_id != -1) {
    // Load the first frame and use the initialization region to initialize the tracker.
    BoundingBox new_init_box = BoundingBox(closest_person_detection[3] * img.cols, 
      closest_person_detection[4] * img.rows, 
      closest_person_detection[5] * img.cols, 
      closest_person_detection[6] * img.rows);  
    
    // imshow("img to feed to tracker:", img);
    // waitKey(0);
    tracker.Init(img, new_init_box, &regressor);
    new_init_box.crop_against_width_height(img.size().width, img.size().height);

    // cout << "new_init_box, x1_:" << new_init_box.x1_ 
    // << ", y1_:" << new_init_box.y1_ 
    // << ", x2_:" << new_init_box.x2_ 
    // << ", y2_:" << new_init_box.y2_ << endl;

    // visualise only the detection
    new_init_box.Draw(0, 255, 0, &img_visualise, 3);

    (*tracker_initialised) = true;
  }
  else if ((*tracker_initialised) && closest_person_detection_id != -1) {
    BoundingBox bbox_estimate;

    // imshow("img to feed to tracker:", img);
    // waitKey(0);
    tracker.Track(img_track, &regressor, &bbox_estimate); //TODO: check why feeding img here does not work!!! compare img and image_track numerically

    // check if the bbox_estimate and closest_person_detection differ too much
    BoundingBox detection_bbox = BoundingBox(closest_person_detection[3] * img.cols, 
      closest_person_detection[4] * img.rows, 
      closest_person_detection[5] * img.cols, 
      closest_person_detection[6] * img.rows); 
    
    // if good detection but the tracking box diverges, reinit
    if (DetectionTrackingDisagree(bbox_estimate, detection_bbox) && closest_person_detection_id != -1) {
      // reinitialise the tracker to the detection
      // cout << "Re init tracker at frame: " << frame_count << endl;
      tracker.Init(img_track, detection_bbox, &regressor);
    }

    // visaulise both the detection and tracking result
    detection_bbox.Draw(0, 255, 0, &img_visualise, 3);
    bbox_estimate.Draw(255, 0, 0, &img_visualise, 3);

    // detect people, turn to people
    // printf("Start sending signal to controller!\n");
    double image_area = img_track.size().width * img_track.size().height;
    double bbox_area_fraction = bbox_estimate.compute_area() / image_area;
    if (bbox_area_fraction > STOP_AREA_TH) {
      // do not turn or proceed, send stop command
      int standval_update = 1;
      int f= controller.SendtoController(turnval, speedval, sitval, standval_update, walkval);
    }
    else {
      // send turn command
      float turnval_update = (bbox_estimate.x1_ + bbox_estimate.x2_)/float(img.cols)/2.0 - 1/2.0;
      turnval_update *= 2;
      int walkval_update = 1;
      int f= controller.SendtoController(turnval_update, speedval, sitval, standval, walkval_update);
    }
  } 
  else if ((*tracker_initialised) && best_person_confidence > PERSON_EXIST_CONFIDENCE_TH) {
    // no confident detection but still have some detection and tracker initialised, still do tracking and use tracking result
    BoundingBox bbox_estimate;
    tracker.Track(img_track, &regressor, &bbox_estimate); //TODO: check why feeding img here does not work!!! compare img and image_track numerically
    // visaulise just the tracker result
    bbox_estimate.Draw(255, 0, 0, &img_visualise, 3);

    // send command using Tracker Result
    double image_area = img_track.size().width * img_track.size().height;
    double bbox_area_fraction = bbox_estimate.compute_area() / image_area;
    if (bbox_area_fraction > STOP_AREA_TH) {
      // do not turn or proceed, send stop command
      int standval_update = 1;
      int f= controller.SendtoController(turnval, speedval, sitval, standval_update, walkval);
    }
    else {
      // send turn command
      float turnval_update = (bbox_estimate.x1_ + bbox_estimate.x2_)/float(img.cols)/2.0 - 1/2.0;
      turnval_update *= 6;
      int walkval_update = 1;
      int f= controller.SendtoController(turnval_update, speedval, sitval, standval, walkval_update);
    }
  }
  else {
    // no detection, no tracking
    // send reset command
    // printf("No people detected!\n");
    int standval_update = 1;
    int f= controller.SendtoController(turnval, speedval, sitval, standval_update, walkval);
  }

  cv::imshow("img to feed to tracker:", img_visualise);
  cv::waitKey(1);

  if (save) {
    // save it
    if (video_writer.isOpened()) {
      video_writer.write(img_visualise);
    }
  }
}


void processDetectionTracking(cv::VideoCapture &cap, Detector &detector, Regressor &regressor, Tracker &tracker, 
  std::string &file, std::ostream &out, float confidence_threshold, const std::string & out_video_path, const bool save = true) {
  VideoWriter video_writer;

  if (!cap.isOpened()) {
      LOG(FATAL) << "Failed to open cap " << endl;
  }

  cv::Mat img;
  int frame_count = 0;

  bool tracker_initialised = false;
  
  controller.DyInit();

  while (true) {
    bool success = cap.read(img);
    if (!success) {
      LOG(INFO) << "End of Video Capture" << endl;
      break;
    }

    // initialise the writer
    if (save) {
      if (frame_count == 0) {
        // Open a video_writer object to save the tracking videos.
        video_writer.open(out_video_path, CV_FOURCC('M','J','P','G'), 20, img.size());
      }
    }

    // process this current frame
    DetectionTrackingProcessFrame(img, frame_count, detector, regressor, tracker, video_writer, 
                                   confidence_threshold, &tracker_initialised, save);

    ++frame_count;
  }

}

void processDetectionTrackingOffline(Video &video, Detector &detector, Regressor &regressor, Tracker &tracker, 
  std::string &file, std::ostream &out, float confidence_threshold, const std::string & out_video_path, const bool save = true) {
  VideoWriter video_writer;

  cv::Mat img;
  BoundingBox bbox_gt;
  int frame_count = 0;
  bool tracker_initialised = false;

  for (int i =0; i< video.all_frames.size(); i ++) {
    bool has_annotation = video.LoadFrame(i,
                                            false,
                                            false,
                                            &img, &bbox_gt);
    // process this current frame
    DetectionTrackingProcessFrame(img, frame_count, detector, regressor, tracker, video_writer, 
                                   confidence_threshold, &tracker_initialised, save);

    ++frame_count;
  }

}

void processDetectionTrackingFromFile(std::string &image_path, Detector &detector, Regressor &regressor, Tracker &tracker, 
  std::string &file, std::ostream &out, float confidence_threshold, const std::string & out_video_path, const bool save = true) {
  VideoWriter video_writer;

  cv::Mat img;
  int frame_count = 0;

  bool tracker_initialised = false;

  while (true) {
    // std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_MICROSECONDS));
    // cout << "Attempt to read from " << image_path << endl;
    img = cv::imread(image_path);

    // cout << "img.empty(): " << img.empty() << endl;
    // cout << "img.size(): " << img.size() << endl;
    if(img.empty() || img.size().width == 0 || img.size().height == 0) {
      continue;
    }

    // process this current frame
    DetectionTrackingProcessFrame(img, frame_count, detector, regressor, tracker, video_writer, 
                                   confidence_threshold, &tracker_initialised, save);

    ++frame_count;
  }

}


DEFINE_string(mean_file, "",
    "The mean file used to subtract from the input image.");
DEFINE_string(mean_value, "104,117,123",
    "If specified, can be one value or can be same as image channels"
    " - would subtract from the corresponding channel). Separated by ','."
    "Either mean_file or mean_value should be provided, not both.");
DEFINE_string(file_type, "image",
    "The file type in the list_file. Currently support image and video.");
DEFINE_string(out_file, "",
    "If provided, store the detection results in the out_file.");
DEFINE_double(confidence_threshold, 0.01,
    "Only store detections with score higher than the threshold.");
DEFINE_int32(gpu_id, 0,
    "the gpu to run on");



int main(int argc, char** argv) {
  ::google::InitGoogleLogging(argv[0]);
  // Print output to stderr (while still logging)
  FLAGS_alsologtostderr = 1;

#ifndef GFLAGS_GFLAGS_H_
  namespace gflags = ::google;
#endif

  gflags::SetUsageMessage("Do detection using SSD mode.\n"
        "Usage:\n"
        "    ssd_detect [FLAGS] model_file weights_file tracker_model tracker_weights list_file out_video_path\n");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const string& model_file = argv[1];
  const string& weights_file = argv[2];
  const string& mean_file = FLAGS_mean_file;
  const string& mean_value = FLAGS_mean_value;
  const string& file_type = FLAGS_file_type;
  const string& out_file = FLAGS_out_file;
  const float confidence_threshold = FLAGS_confidence_threshold;

  // Initialize the network.
  Detector detector(model_file, weights_file, mean_file, mean_value);

  // Set the output mode.
  std::streambuf* buf = std::cout.rdbuf();
  std::ofstream outfile;
  if (!out_file.empty()) {
    outfile.open(out_file.c_str());
    if (outfile.good()) {
      buf = outfile.rdbuf();
    }
  }
  std::ostream out(buf);


  // For Tracker
  const string& tracker_model_file   = argv[3];
  const string& tracker_trained_file = argv[4];

  const int gpu_id = FLAGS_gpu_id;

  const bool do_train = false;
  Regressor regressor(tracker_model_file, tracker_trained_file, gpu_id, do_train);

  // Ensuring randomness for fairness.
  // srandom(800);
  srandom(time(NULL));

  // Create a tracker object.
  const bool show_intermediate_output = false;
  Tracker tracker(show_intermediate_output);

  // Process image one by one.
  std::ifstream infile(argv[5]);
  std::string file;

  const std::string& out_video_path = argv[6];
  cout << "out_video_path: "<< out_video_path << endl;

  while (infile >> file) {
    if (file_type == "image") {
      cv::Mat img = cv::imread(file, -1);
      CHECK(!img.empty()) << "Unable to decode image " << file;
      std::vector<vector<float> > detections = detector.Detect(img);

      /* Print the detection results. */
      for (int i = 0; i < detections.size(); ++i) {
        const vector<float>& d = detections[i];
        // Detection format: [image_id, label, score, xmin, ymin, xmax, ymax].
        CHECK_EQ(d.size(), 7);
        const float score = d[2];
        if (score >= confidence_threshold) {
          out << file << " ";
          out << static_cast<int>(d[1]) << " ";
          out << score << " ";
          out << static_cast<int>(d[3] * img.cols) << " ";
          out << static_cast<int>(d[4] * img.rows) << " ";
          out << static_cast<int>(d[5] * img.cols) << " ";
          out << static_cast<int>(d[6] * img.rows) << std::endl;
        }
      }
    } else if (file_type == "video") {
      cv::VideoCapture cap(file);
      processDetectionTracking(cap, detector, regressor, tracker, file, out, confidence_threshold, out_video_path);
      // close capture stream
      if (cap.isOpened()) {
        cap.release();
      }
    }
    else if (file_type == "webcam") {
      cv::VideoCapture cap(0); // default webcam id
      processDetectionTracking(cap, detector, regressor, tracker, file, out, confidence_threshold, out_video_path, false);
      // close capture stream
      if (cap.isOpened()) {
        cap.release();
      }
    }
    else if (file_type == "videos_folder") {
        std::vector<Video> videos;
        LoaderVOT loader(file);
        videos = loader.get_videos();

        for (int i = 0; i < videos.size(); i++) {
          processDetectionTrackingOffline(videos[i], detector, regressor, tracker, file, out, confidence_threshold, out_video_path);
        }
    }
    else if (file_type == "from_file") {
      string image_path = "/home/sharon/work/tracker/build/ImageOriginal.bmp";
      processDetectionTrackingFromFile(image_path, detector, regressor, tracker, file, out, confidence_threshold, out_video_path);
    }
    else {
      LOG(FATAL) << "Unknown file_type: " << file_type;
    }
  }
  return 0;
}
#else
int main(int argc, char** argv) {
  LOG(FATAL) << "This example requires OpenCV; compile with USE_OPENCV.";
}
#endif  // USE_OPENCV
