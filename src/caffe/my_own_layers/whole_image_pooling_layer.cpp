// ------------------------------------------------------------------
// MY OWN
// Copyright (c) 2016 DDK
// Licensed under The MIT License
// Written by DengKe Dong
// ------------------------------------------------------------------

#include <cfloat>

#include "caffe/my_own_layers.hpp"

using std::max;
using std::min;
using std::floor;
using std::ceil;

namespace caffe {

template <typename Dtype>
void WholeImagePoolingLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) 
{
  ROIPoolingParameter roi_pool_param = this->layer_param_.roi_pooling_param();
  CHECK_GT(roi_pool_param.pooled_h(), 0)
      << "pooled_h must be > 0";
  CHECK_GT(roi_pool_param.pooled_w(), 0)
      << "pooled_w must be > 0";
  pooled_height_ = roi_pool_param.pooled_h();
  pooled_width_  = roi_pool_param.pooled_w();
  spatial_scale_ = roi_pool_param.spatial_scale();
  LOG(INFO) << "Spatial scale: " << spatial_scale_;
}

template <typename Dtype>
void WholeImagePoolingLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) 
{
  int num   = bottom[0]->num();
  channels_ = bottom[0]->channels();
  height_   = bottom[0]->height();
  width_    = bottom[0]->width();

  top[0]->Reshape( num, channels_, pooled_height_, pooled_width_);
  max_idx_.Reshape(num, channels_, pooled_height_, pooled_width_);
  bboxes_.Reshape( num, 5, 1, 1);

  /*
    if keep height/width ratio, it possibly uses zero-padding to fullfill the input
    so it may not reasonably for the parts of padding, 
    when pooling and batchsize is larger than 1. 
  */
  const Dtype* im_info = NULL;
  const bool flag      = bottom.size() == 2;
  if(flag) {
    CHECK_EQ(bottom[1]->count() / bottom[1]->num(), 5) << "wrong `im_info` params";
    im_info            = bottom[1]->cpu_data();
  }
  // set rois, where they are the whole images
  Dtype* bottom_rois = bboxes_.mutable_cpu_data();

  for (int n = 0; n < num; ++n) {
    bottom_rois[0] = Dtype(n);
    bottom_rois[1] = Dtype(0);
    bottom_rois[2] = Dtype(0);

    if(flag) {
      /// im_ind, height, width, scale, flippable
      /// only need origin_height, origin_width, and scale
      //  get offset of coordinates
      const int offset     = bottom[1]->offset(n);
      const Dtype o_width  = im_info[offset + 1];
      const Dtype o_height = im_info[offset + 2];
      const Dtype o_scale  = im_info[offset + 3];

      bottom_rois[3] = Dtype(o_width  * o_scale - 1);
      bottom_rois[4] = Dtype(o_height * o_scale - 1);
    } else {
      bottom_rois[3] = Dtype(width_  - 1);
      bottom_rois[4] = Dtype(height_ - 1);
    }
    
    bottom_rois   += bboxes_.offset(1);
  }
}

template <typename Dtype>
void WholeImagePoolingLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->cpu_data();
  const Dtype* bottom_rois = bboxes_.cpu_data();

  // Number of ROIs
  int num_rois     = bboxes_.num();
  int batch_size   = bottom[0]->num();
  CHECK_EQ(num_rois, batch_size);

  int top_count    = top[0]->count();
  Dtype* top_data  = top[0]->mutable_cpu_data();
  caffe_set(top_count, Dtype(-FLT_MAX), top_data);
  int* argmax_data = max_idx_.mutable_cpu_data();
  caffe_set(top_count, -1, argmax_data);

  // For each ROI R = [batch_index x1 y1 x2 y2]: max pool over R
  for (int n = 0; n < num_rois; ++n) {
    int roi_batch_ind = bottom_rois[0];
    int roi_start_w   = round(bottom_rois[1] * spatial_scale_);
    int roi_start_h   = round(bottom_rois[2] * spatial_scale_);
    int roi_end_w     = round(bottom_rois[3] * spatial_scale_);
    int roi_end_h     = round(bottom_rois[4] * spatial_scale_);
    CHECK_GE(roi_batch_ind, 0);
    CHECK_LT(roi_batch_ind, batch_size);

    int roi_height = max(roi_end_h - roi_start_h + 1, 1);
    int roi_width = max(roi_end_w - roi_start_w + 1, 1);
    const Dtype bin_size_h = static_cast<Dtype>(roi_height)
                             / static_cast<Dtype>(pooled_height_);
    const Dtype bin_size_w = static_cast<Dtype>(roi_width)
                             / static_cast<Dtype>(pooled_width_);

    const Dtype* batch_data = bottom_data + bottom[0]->offset(roi_batch_ind);

    for (int c = 0; c < channels_; ++c) {
      for (int ph = 0; ph < pooled_height_; ++ph) {
        for (int pw = 0; pw < pooled_width_; ++pw) {
          // Compute pooling region for this output unit:
          //  start (included) = floor(ph * roi_height / pooled_height_)
          //  end (excluded) = ceil((ph + 1) * roi_height / pooled_height_)
          int hstart = static_cast<int>(floor(static_cast<Dtype>(ph)
                                              * bin_size_h));
          int wstart = static_cast<int>(floor(static_cast<Dtype>(pw)
                                              * bin_size_w));
          int hend = static_cast<int>(ceil(static_cast<Dtype>(ph + 1)
                                           * bin_size_h));
          int wend = static_cast<int>(ceil(static_cast<Dtype>(pw + 1)
                                           * bin_size_w));

          hstart = min(max(hstart + roi_start_h, 0), height_);
          hend   = min(max(hend + roi_start_h, 0), height_);
          wstart = min(max(wstart + roi_start_w, 0), width_);
          wend   = min(max(wend + roi_start_w, 0), width_);

          bool is_empty = (hend <= hstart) || (wend <= wstart);

          const int pool_index = ph * pooled_width_ + pw;
          if (is_empty) {
            top_data[pool_index]    = 0;
            argmax_data[pool_index] = -1;
          }

          for (int h = hstart; h < hend; ++h) {
            for (int w = wstart; w < wend; ++w) {
              const int index = h * width_ + w;
              if (batch_data[index] > top_data[pool_index]) {
                top_data[pool_index]    = batch_data[index];
                argmax_data[pool_index] = index;
              }
            }
          }
        }
      }
      // Increment all data pointers by one channel
      batch_data  += bottom[0]->offset(0, 1);
      top_data    += top[0]->offset(0, 1);
      argmax_data += max_idx_.offset(0, 1);
    }
    // Increment ROI data pointer
    bottom_rois += bboxes_.offset(1);
  }
}

template <typename Dtype>
void WholeImagePoolingLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  NOT_IMPLEMENTED;
}


#ifdef CPU_ONLY
STUB_GPU(WholeImagePoolingLayer);
#endif

INSTANTIATE_CLASS(WholeImagePoolingLayer);
REGISTER_LAYER_CLASS(WholeImagePooling);

}  // namespace caffe