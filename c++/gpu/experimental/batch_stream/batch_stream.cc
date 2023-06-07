#include <chrono>
#include <iostream>
#include <memory>
#include <numeric>

#include <cuda_runtime.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "paddle_inference_api.h"

using paddle_infer::Config;
using paddle_infer::CreatePredictor;
using paddle_infer::PrecisionType;
using paddle_infer::Predictor;

DEFINE_string(model_file, "", "Directory of the inference model.");
DEFINE_string(params_file, "", "Directory of the inference model.");
DEFINE_string(model_dir, "", "Directory of the inference model.");
DEFINE_int32(batch_size, 1, "Directory of the inference model.");
DEFINE_int32(warmup, 0, "warmup.");
DEFINE_int32(repeats, 1, "repeats.");

using Time = decltype(std::chrono::high_resolution_clock::now());
Time time() { return std::chrono::high_resolution_clock::now(); };
double time_diff(Time t1, Time t2) {
  typedef std::chrono::microseconds ms;
  auto diff = t2 - t1;
  ms counter = std::chrono::duration_cast<ms>(diff);
  return counter.count() / 1000.0;
}

std::shared_ptr<Predictor> InitPredictor(cudaStream_t stream) {
  Config config;
  if (FLAGS_model_dir != "") {
    config.SetModel(FLAGS_model_dir);
  }
  config.SetModel(FLAGS_model_file, FLAGS_params_file);
  config.EnableUseGpu(500, 0);

  config.SetExecStream(stream);

  // Open the memory optim.
  config.EnableMemoryOptim();
  return CreatePredictor(config);
}

void run(Predictor *predictor, const std::vector<float> &input,
         const std::vector<int> &input_shape, std::vector<float> *out_data,
         cudaStream_t stream) {
  int input_num = std::accumulate(input_shape.begin(), input_shape.end(), 1,
                                  std::multiplies<int>());

  auto input_names = predictor->GetInputNames();
  auto output_names = predictor->GetOutputNames();
  auto input_t = predictor->GetInputHandle(input_names[0]);
  input_t->Reshape(input_shape);
  input_t->CopyFromCpu(input.data());

  for (size_t i = 0; i < FLAGS_warmup; ++i) {
    CHECK(paddle_infer::experimental::InternalUtils::RunWithExternalStream(
        predictor, stream));
  }

  auto st = time();
  for (size_t i = 0; i < FLAGS_repeats; ++i) {
    CHECK(paddle_infer::experimental::InternalUtils::RunWithExternalStream(
        predictor, stream));
    auto output_t = predictor->GetOutputHandle(output_names[0]);
    std::vector<int> output_shape = output_t->shape();
    int out_num = std::accumulate(output_shape.begin(), output_shape.end(), 1,
                                  std::multiplies<int>());
    out_data->resize(out_num);
    output_t->CopyToCpu(out_data->data());
  }
  LOG(INFO) << "run avg time is " << time_diff(st, time()) / FLAGS_repeats
            << " ms, stream " << predictor->GetExecStream();
}

int main(int argc, char *argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  cudaStream_t stream;
  cudaStreamCreate(&stream);
  auto predictor = InitPredictor(stream);
  std::vector<int> input_shape = {FLAGS_batch_size, 3, 224, 224};
  std::vector<float> input_data(FLAGS_batch_size * 3 * 224 * 224);
  for (size_t i = 0; i < input_data.size(); ++i)
    input_data[i] = i % 255 * 0.1;
  std::vector<float> out_data;

  int stream_num = 4;
  std::vector<cudaStream_t> streams(stream_num);
  for (size_t i = 0; i < stream_num; ++i) {
    cudaStreamCreate(&streams[i]);
  }
  for (size_t i = 0; i < stream_num; ++i)
    run(predictor.get(), input_data, input_shape, &out_data, streams[i]);

  return 0;
}
