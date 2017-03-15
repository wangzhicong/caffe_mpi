#include <glog/logging.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#include "boost/algorithm/string.hpp"
#include "caffe/caffe.hpp"
#include "caffe/common.hpp"
#include "opencv2/opencv.hpp"
#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"


#ifdef DTYPE_FP16
#include "caffe/half/half.hpp"
#define Dtype Half
#else
#define Dtype float
#endif

#ifdef USE_MOG
#include "caffe/util/MogControl.h"
#endif

//#include "OF_DIS/gen_flow.h"

using caffe::Blob;
using caffe::Caffe;
using caffe::Net;
using caffe::Layer;
using caffe::shared_ptr;
using caffe::Timer;
using caffe::vector;
using namespace cv;


const float flow_th = 1.0f;
const float rgb_th = 0.002f;
const int grid_size = 15;
const float momentum_value = 0.0f;
const int duration = 1;
const int choose_num = 3;

// DEFINE_int32(gpu, 0,
//     "Run in GPU mode on given device ID.");
DEFINE_string(model, "",
    "The model definition protocol buffer text file..");
DEFINE_string(weights, "",
    "The pretrained weights to initialize finetuning. "
    "Cannot be set simultaneously with snapshot.");
DEFINE_int32(shrink, 8,
    "Optional; Supported interp shrink ratio.");
DEFINE_int32(show_channel, 1,
    "Optional; Mask input with channel id #showchannel.");
DEFINE_string(output_blob, "prob",
    "Optional; The blob name for the output probability in network proto.");
DEFINE_string(video_input, "/DATA/video/segment/",
    "Optional; Input video path, or from camera.");
DEFINE_string(video_input_file, "segment_list.txt",
    "Optional; Input video path, or from camera.");
DEFINE_string(video_out, "/DATA/video/segment_label/",
    "output video file (postfix .avi is added).");
DEFINE_int32(fps, 24,
  "fps of output video");
DEFINE_int32(id, 0,
  "thread id");
DEFINE_int32(tot_id, 8,
  "total thread id");

class HumanPredictor {
public:
  HumanPredictor() {};
  ~HumanPredictor() {};
  bool Init(const std::string &net_file, const std::string &model_file, const bool is_GPU,
   const float* mean_ptr,  const std::string &output_blob, int shrink);
  bool PredictProb(const cv::Mat& input_frame, std::vector<cv::Mat>& human_prob_mat) const;
  void Reshape(int height, int width);
  bool PredictProb(const cv::Mat& input_frame, std::vector<cv::Mat>& human_prob_mat, std::vector<cv::Mat>& human_prob_mat_f, cv::Mat& human_mask_mat) const;
  vector<int> PadParam(int video_cols, int video_rows);
  Mat show_mask(const cv::Mat&  img_ori, const cv::Mat& prob_mat, int show_channel);
  inline int get_height_in() { return height_network_input; }
  inline int get_width_in() { return width_network_input; }
  inline int get_resize_width() { return resize_width_video; }
  inline int get_resize_height() { return resize_height_video; }
  inline int get_channel_network_prob() { return channel_network_prob; }


private:
  caffe::Net<float> *model;
  int height_network_input = 0;
  int width_network_input = 0;
  int height_network_prob = 0;
  int width_network_prob = 0;
  int channel_network_prob = 0;
  int resize_height_video = 0;
  int resize_width_video = 0;
  int m_shrink = 0;
  float mean[3]; // bgr mean
  string output_blob_name;
};

bool HumanPredictor::Init(const std::string &net_file, const std::string &model_file,
  const bool is_GPU, const float* mean_ptr, const std::string &output_blob, int shrink) {
  if (is_GPU >= 0) {
    LOG(INFO) << "Use GPU with device ID " << is_GPU;
    Caffe::SetDevice(is_GPU);
    Caffe::set_mode(Caffe::GPU);
  } else {
    LOG(INFO) << "Use CPU.";
    Caffe::set_mode(Caffe::CPU);
  }

  model = new caffe::Net<float>(net_file, caffe::TEST);
  if (model == NULL) { return false; }
  model->CopyTrainedLayersFrom(model_file);

  output_blob_name = output_blob;
  boost::shared_ptr<caffe::Blob<float> > data_blob = model->blob_by_name("data");
  boost::shared_ptr<caffe::Blob<float> > prob_blob = model->blob_by_name(output_blob_name);

  m_shrink = shrink;

  CHECK_EQ(data_blob->num(), 1);
  CHECK_EQ(data_blob->channels(), 3);
  height_network_input = data_blob->height();
  width_network_input = data_blob->width();
  CHECK_GT(height_network_input, 0);
  CHECK_GT(width_network_input, 0);

  CHECK_EQ(prob_blob->num(), 1);
  CHECK_EQ(prob_blob->channels(), 2);
  height_network_prob = prob_blob->height();
  width_network_prob = prob_blob->width();
  channel_network_prob = prob_blob->channels();
  CHECK_GT(height_network_prob, 0);
  CHECK_GT(width_network_prob, 0);

  for (size_t c = 0; c < 3; c++) {
    mean[c] = mean_ptr[c];
  }

  LOG(INFO) << "predictor init finished";
  return true;
}

void HumanPredictor::Reshape(int height, int width){
  model->blob_by_name("data")->Reshape(1, 3, height, width);
  model->Reshape();
  boost::shared_ptr<caffe::Blob<float> > prob_blob = model->blob_by_name(output_blob_name);
  height_network_input = height;
  width_network_input = width;
  height_network_prob = prob_blob->height();
  width_network_prob = prob_blob->width();
}

vector<int> HumanPredictor::PadParam(int video_cols, int video_rows){ 
  float scale = std::min((float)width_network_input / video_cols, (float)height_network_input / video_rows);

  resize_width_video = video_cols * scale;
  resize_height_video = video_rows * scale;

  vector<int> pad_vector(4, 0);
  pad_vector[0] = (height_network_input - resize_height_video) / 2;
  pad_vector[1] = height_network_input - resize_height_video - pad_vector[0];
  pad_vector[2] = (width_network_input - resize_width_video) / 2;
  pad_vector[3] = width_network_input - resize_width_video - pad_vector[2];

  LOG(INFO) << "Video Input: " << video_rows << " " << video_cols;
  LOG(INFO) << "Resized Video Input: " << resize_height_video << " " << resize_width_video;
  LOG(INFO) << "Pad Video Input: " << height_network_input << " " << width_network_input;
  LOG(INFO) << "Pad top/bottom/left/right: " << pad_vector[0] << " " << pad_vector[1] << " " << pad_vector[2] << " " << pad_vector[3];

  return pad_vector;
}

bool HumanPredictor::PredictProb(const cv::Mat& input_frame, std::vector<cv::Mat>& human_prob_mat) const
{
  boost::shared_ptr<caffe::Blob<float> > data_blob = model->blob_by_name("data");
  boost::shared_ptr<caffe::Blob<float> > prob_blob = model->blob_by_name(output_blob_name);

  CHECK_EQ(input_frame.rows, height_network_input);
  CHECK_EQ(input_frame.cols, width_network_input);
  CHECK_EQ(input_frame.channels(), 3);

  int output_channels = prob_blob->channels();
  human_prob_mat.resize(output_channels);
  for (int i = 0; i < output_channels; i++) {
    human_prob_mat[i] = Mat(height_network_prob, width_network_prob, CV_32FC1);
  }

  Mat bgrFrame[3];
  split(input_frame, bgrFrame);
  for (size_t c = 0; c < 3; c++) {
    bgrFrame[c].convertTo(bgrFrame[c], CV_32F);
    bgrFrame[c] = bgrFrame[c] - mean[c];
  }

  float* data_blob_data = data_blob->mutable_cpu_data();
  for (size_t c = 0; c < 3; c++) {
    memcpy(data_blob_data + c * height_network_input * width_network_input,
        bgrFrame[c].data, sizeof(float) * height_network_input * width_network_input);
  }

  Timer forward_timer;
  forward_timer.Start();
  model->ForwardPrefilled();
  forward_timer.Stop();
  LOG(INFO) << "Forward Time: " << forward_timer.MilliSeconds() << " ms.";

  const float* prob_blob_data = prob_blob->cpu_data();

  for (int c = 0; c < output_channels; c++) {
    memcpy(human_prob_mat[c].data, prob_blob_data + 
      c * height_network_prob * width_network_prob, 
      sizeof(float) * height_network_prob * width_network_prob);
  }
  return true;
}

Mat HumanPredictor::show_mask(const cv::Mat& img_ori, const cv::Mat& prob_mat, int show_channel) {
  Mat prob_img, prob_mat1;
  resize(prob_mat, prob_mat1, Size(img_ori.cols, img_ori.rows));
  CHECK_EQ(img_ori.rows, prob_mat1.rows);
  CHECK_EQ(img_ori.cols, prob_mat1.cols);

  CHECK_EQ(img_ori.channels(), 3);
  CHECK_EQ(prob_mat.channels(), 1);

  vector<Mat> split_channels;
  split(img_ori, split_channels);
  show_channel =  show_channel;
  for (int i = 0; i < img_ori.rows * img_ori.cols; i++){
    split_channels[1].at<uchar>(i) = prob_mat1.at<float>(i) == show_channel ?
        split_channels[1].at<uchar>(i) : 0;
  }
  merge(split_channels, prob_img);

  return prob_img;
}

bool mk_file_dir(string filename) {
  int path_len = 0;
  struct stat statbuf;
  int dir_err = 0;
  string path;
  for (string::iterator it=filename.begin()+1; it!=filename.end(); ++it) {
    path_len++;
    if (*it=='/') {
      path = filename.substr(0, path_len);
      char *cpath = new char [path.length()+1];
      strcpy(cpath, path.c_str());
      if (stat(cpath, &statbuf) != -1) {
        if (!S_ISDIR(statbuf.st_mode)) {}
      }
      else {
        dir_err = mkdir(cpath, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if (dir_err < 0) {
          return false;
        }
      }
      delete[] cpath;
    }
  }
  return true;
}

int median(std::vector<int> &input)
{
  std::vector<int> temp(input);
  sort(temp.begin(), temp.end());
  return temp[int(temp.size() / 2)];
}

inline bool exists_test (const std::string& name) {
    if (FILE *file = fopen(name.c_str(), "r")) {
        fclose(file);
        return true;
    } else {
        return false;
    }   
}

int main(int argc, char** argv) {
  // Initialization
  
  const vector<float> scales = {256-64, 256, 256+64};   

  caffe::GlobalInit(&argc, &argv);

  // CNN Initilization
  const float mean[3] = {103.939, 116.779, 123.68};
  string proto_model = FLAGS_model;
  string weights = FLAGS_weights;
  CHECK(!proto_model.empty()) << "Model prototxt needed.";
  CHECK(!weights.empty()) << "Trained model file needed.";
  int video_cols = 576;
  int video_rows = 1024;

  HumanPredictor predictor;
  CHECK(predictor.Init(proto_model, weights, 0, mean, FLAGS_output_blob, FLAGS_shrink));

  FILE *fid = fopen((FLAGS_video_input + FLAGS_video_input_file).c_str(), "r");


  char buff[200];
  int have_done = -1, line_num = -1;
  while (fscanf(fid, "%s\n", buff) != EOF)
  {
    if (++line_num % FLAGS_tot_id != FLAGS_id)
      continue;
    string temp = std::string(buff).substr(0, strlen(buff) - 4);
    if (!exists_test(FLAGS_video_out + temp + "/0.png"))
      break;   
    have_done = line_num;
  }
  fclose(fid);

  fid = fopen((FLAGS_video_input + FLAGS_video_input_file).c_str(), "r");
  line_num = -1; 
  while (fscanf(fid, "%s\n", buff) != EOF)
  {
    if (++line_num % FLAGS_tot_id != FLAGS_id || line_num < have_done)
      continue;

    string temp = std::string(buff).substr(0, strlen(buff) - 4);
    mk_file_dir((FLAGS_video_out + temp + "/").c_str());
    LOG(ERROR) << buff;



    VideoCapture capture;
    capture.open(FLAGS_video_input + std::string(buff));  // Open from file
    if (!capture.isOpened()) {
      fprintf(stderr, "can not open camera or file not exist!\n");
      break;
      continue;
    }


    //CHECK(mk_file_dir((FLAGS_video_out + std::string(buff)).c_str())) << "mkdir error: " << buff;
    //VideoWriter vw;
    //vw.open(FLAGS_video_out + std::string(buff), CV_FOURCC('M', 'J', 'P', 'G'), FLAGS_fps,
    //      cv::Size(video_cols, video_rows), true);


    Mat read_frame, temp_frame; int cnt = 0;
    while (capture.read(read_frame)) {
      
      vector<Mat> total_human_prob_mat(2);
      for (int j = 0; j < 2; j++) {
        total_human_prob_mat[j] = Mat(video_rows, video_cols, CV_32FC1, Scalar(0));
      }

      for (int i=0; i<scales.size(); i++)
      {
        read_frame.copyTo(temp_frame);
        predictor.Reshape(((int)round(scales[i] / 576 * 1024)) / 32*32 , scales[i]);
        vector<int> pad = predictor.PadParam(video_cols, video_rows);

        vector<Mat> human_prob_mat;

        resize(temp_frame, temp_frame, Size(predictor.get_resize_width(), predictor.get_resize_height()));
        
        copyMakeBorder(temp_frame, temp_frame, pad[0], pad[1], pad[2], pad[3], 
         BORDER_CONSTANT, Scalar(mean[0], mean[1], mean[2]) );
        
        predictor.PredictProb(temp_frame, human_prob_mat);
        cv::Rect ROI(pad[2], pad[0], predictor.get_resize_width(), predictor.get_resize_height());
        resize(human_prob_mat[0](ROI), human_prob_mat[0], Size(video_cols, video_rows));
        resize(human_prob_mat[1](ROI), human_prob_mat[1], Size(video_cols, video_rows));
        total_human_prob_mat[0] += human_prob_mat[0];
        total_human_prob_mat[1] += human_prob_mat[1];

        flip(temp_frame, temp_frame, 1);
        predictor.PredictProb(temp_frame, human_prob_mat);
        flip(human_prob_mat[0], human_prob_mat[0], 1);
        flip(human_prob_mat[1], human_prob_mat[1], 1);
        resize(human_prob_mat[0](ROI), human_prob_mat[0], Size(video_cols, video_rows));
        resize(human_prob_mat[1](ROI), human_prob_mat[1], Size(video_cols, video_rows));
        total_human_prob_mat[0] += human_prob_mat[0];
        total_human_prob_mat[1] += human_prob_mat[1];
      }

      Mat human_mask_mat = Mat(video_rows, video_cols, CV_8UC1, Scalar(0));
      for (int i = 0; i < video_rows * video_cols; i++){
        float prob = total_human_prob_mat[0].at<float>(i);
        for (int c = 1; c < 2; c++){
          if (total_human_prob_mat[c].at<float>(i) > prob){
            prob = total_human_prob_mat[c].at<float>(i);
          }
        }
        human_mask_mat.at<uchar>(i) = (int)(total_human_prob_mat[1].at<float>(i) / (scales.size() * 2.0) * 255.0);
      }
      //LOG(ERROR) << FLAGS_video_out + temp + "/" + std::to_string(cnt)+".png";
      cv::imwrite(FLAGS_video_out + temp + "/" + std::to_string(cnt++)+".png", human_mask_mat);
      //cv::hconcat(read_frame, human_mask_mat, read_frame);
      //cv::imshow("Img", read_frame);
      //waitKey(1);
    }
    capture.release();
  }
  LOG(INFO) << "Finished!";
  return 0;
}
