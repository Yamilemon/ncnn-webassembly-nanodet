// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <benchmark.h>
#include <simpleocv.h>
#include "nanodet.h"

using namespace std;

static int draw_fps(cv::Mat& rgba)
{
    // resolve moving average
    float avg_fps = 0.f;
    {
        static double t0 = 0.f;
        static float fps_history[10] = {0.f};

        double t1 = ncnn::get_current_time();
        if (t0 == 0.f)
        {
            t0 = t1;
            return 0;
        }

        float fps = 1000.f / (t1 - t0);
        t0 = t1;

        for (int i = 9; i >= 1; i--)
        {
            fps_history[i] = fps_history[i - 1];
        }
        fps_history[0] = fps;

        if (fps_history[9] == 0.f)
        {
            return 0;
        }

        for (int i = 0; i < 10; i++)
        {
            avg_fps += fps_history[i];
        }
        avg_fps /= 10.f;
    }

    char text[32];
    sprintf(text, "FPS=%.2f", avg_fps);

    int baseLine = 0;
    cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

    int y = 0;
    int x = rgba.cols - label_size.width;

    cv::rectangle(rgba, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                    cv::Scalar(255, 255, 255, 255), -1);

    cv::putText(rgba, text, cv::Point(x, y + label_size.height),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0, 255));

    return 0;
}

static NanoDet* g_nanodet = 0;
char *model = "lightning";
static void on_image_render(cv::Mat& rgba)
{
    if (!g_nanodet)
    {
        int modelid = 0;// 默认使用lightning
        int cpugpu = 1;// 默认使用cpu
                    // 不是lightning的话就使用thunder
        if (strcmp(model, "lightning") != 0) modelid = 1;

        if (modelid < 0 || modelid > 6 || cpugpu < 0 || cpugpu > 1)
        {
            return;
        }

        const char* modeltypes[] =
        {
            "lightning",
            "thunder",
        };

        const int target_sizes[] =
        {
            192,
            256,
        };

        const float mean_vals[][3] =
        {
            { 127.5f, 127.5f,  127.5f },
            { 127.5f, 127.5f,  127.5f },
        };

        const float norm_vals[][3] =
        {
            { 1 / 127.5f, 1 / 127.5f, 1 / 127.5f },
            { 1 / 127.5f, 1 / 127.5f, 1 / 127.5f },
        };

        const char* modeltype = modeltypes[(int)modelid];
        int target_size = target_sizes[(int)modelid];
        bool use_gpu = (int)cpugpu == 1;

        if (!g_nanodet) g_nanodet = new NanoDet;
        g_nanodet->load(modeltype, target_size, mean_vals[(int)modelid], norm_vals[(int)modelid], use_gpu);

        /*g_nanodet = new NanoDet;

        static const float mean_vals[3] = {103.53f, 116.28f, 123.675f};
        static const float norm_vals[3] = {1.f / 57.375f, 1.f / 57.12f, 1.f / 58.395f};

        g_nanodet->load("m", 320, mean_vals, norm_vals);*/
    }

    std::vector<keypoint> points;
	g_nanodet->detect_point(rgba, points);
    g_nanodet->draw(rgba, points);

    // 回调js
    string s = "'[";
	for (int i = 0; i < points.size(); i++) {
		if (i < points.size() - 1) s = s + "{\"x\":" + to_string(w - points[i].x) + ",\"y\":" + to_string(points[i].y) + ",\"score\":" + to_string(points[i].score * 100) + "},";
		else if (i == points.size() - 1) s = s + "{\"x\":" + to_string(w - points[i].x) + ",\"y\":" + to_string(points[i].y) + ",\"score\":" + to_string(points[i].score * 100) + "}]'";
	}
	string pose = "'{}'";
	string fps = "'30'";
	string isMovenet = "'true'";
	string run_js = "if(window.nano_z.onFrame) { window.nano_z.onFrame(" + s + ", "+ pose +", "+ fps +","+ isMovenet +"); }";
    emscripten_run_script(run_js.c_str());

    draw_fps(rgba);
}

#ifdef __EMSCRIPTEN_PTHREADS__

static const unsigned char* rgba_data = 0;
static int w = 0;
static int h = 0;

static ncnn::Mutex lock;
static ncnn::ConditionVariable condition;

static ncnn::Mutex finish_lock;
static ncnn::ConditionVariable finish_condition;

static void worker()
{
    while (1)
    {
        lock.lock();
        while (rgba_data == 0)
        {
            condition.wait(lock);
        }

        cv::Mat rgba(h, w, CV_8UC4, (void*)rgba_data);

        on_image_render(rgba);

        rgba_data = 0;

        lock.unlock();

        finish_lock.lock();
        finish_condition.signal();
        finish_lock.unlock();
    }
}

#include <thread>
static std::thread t(worker);

extern "C" {

void nanodet_ncnn(unsigned char* _rgba_data, int _w, int _h)
{
    lock.lock();
    while (rgba_data != 0)
    {
        condition.wait(lock);
    }

    rgba_data = _rgba_data;
    w = _w;
    h = _h;

    lock.unlock();

    condition.signal();

    // wait for finished
    finish_lock.lock();
    while (rgba_data != 0)
    {
        finish_condition.wait(finish_lock);
    }
    finish_lock.unlock();
}

}

#else // __EMSCRIPTEN_PTHREADS__

extern "C" {

void nanodet_ncnn(unsigned char* rgba_data, int w, int h)
{
    cv::Mat rgba(h, w, CV_8UC4, (void*)rgba_data);

    // 镜像翻转
    //cv::Mat v_frame;
	//cv::flip(rgba, v_frame, 1);

    on_image_render(rgba);
}

}

#endif // __EMSCRIPTEN_PTHREADS__
