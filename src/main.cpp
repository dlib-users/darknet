#include "darknet.h"
#include "ui_utils.h"
#include "weights_visitor.h"
#include "yolov4_sam_mish.h"

#include <dlib/cmd_line_parser.h>
#include <dlib/dir_nav.h>
#include <dlib/image_io.h>

const static std::string exts{".jpg .JPG .jpeg .JPEG .png .PNG .gif .GIF"};

int main(const int argc, const char** argv)
try
{
    dlib::command_line_parser parser;
    parser.add_option("images", "directory with images to process", 1);
    parser.add_option("input", "path to video file to process (defaults to webcam)", 1);
    parser.add_option("output", "path to output video file (.mkv extension) or directory", 1);
    parser.add_option("webcam", "index of webcam to use (default: 0)", 1);
    parser.add_option("names", "path to file with label names (one per line)", 1);
    parser.add_option("img-size", "image size to process (default: 416)", 1);
    parser.add_option("conf-thresh", "confidence threshold (default: 0.25)", 1);
    parser.add_option("nms-thresh", "non-max suppression threshold (default: 0.45)", 1);
    parser.add_option("fps", "force frames per second (default: 30)", 1);
    parser.add_option("print", "print out the network architecture");
    parser.add_option("dnn", "path to dlib saved model", 1);
    parser.add_option("out-width", "set output width", 1);
    parser.set_group_name("Help Options");
    parser.add_option("h", "alias for --help");
    parser.add_option("help", "display this message and exit");
    parser.parse(argc, argv);

    if (parser.option("h") or parser.option("help"))
    {
        parser.print_options();
        webcam_window::print_keyboard_shortcuts();
        return EXIT_SUCCESS;
    }

    parser.check_incompatible_options("images", "input");
    parser.check_incompatible_options("images", "webcam");
    parser.check_incompatible_options("input", "webcam");

    const std::string names_path = dlib::get_option(parser, "names", "");
    const int webcam_idx = dlib::get_option(parser, "webcam", 0);
    float fps = dlib::get_option(parser, "fps", 30);
    const long img_size = dlib::get_option(parser, "img-size", 416);
    const float conf_thresh = dlib::get_option(parser, "conf-thresh", 0.25);
    const float nms_thresh = dlib::get_option(parser, "nms-thresh", 0.45);
    const std::string dnn_path = dlib::get_option(parser, "dnn", "");
    const long out_width = dlib::get_option(parser, "out-width", 0);
    std::vector<std::string> labels;
    if (names_path.empty())
    {
        std::cout << "Please provide a path to the label names file\n";
        return EXIT_FAILURE;
    }
    else
    {
        std::ifstream fin(names_path);
        for (std::string line; std::getline(fin, line);)
        {
            labels.push_back(line);
        }
    }
    std::cout << "found " << labels.size() << " classes\n";

    yolov4_sam_mish yolo(dnn_path, names_path);
    const auto label_to_color = get_color_map(labels);
    webcam_window win;

    if (parser.option("images"))
    {
        const std::string images_dir = parser.option("images").argument();
        const std::string output_dir = dlib::get_option(parser, "output", "detections");
        dlib::create_directory(output_dir);
        for (const auto& file :
             dlib::get_files_in_directory_tree(images_dir, dlib::match_endings(exts)))
        {
            dlib::matrix<dlib::rgb_pixel> image;
            dlib::load_image(image, file.full_name());
            std::vector<detection> detections;
            yolo.detect(image, detections, img_size, conf_thresh, nms_thresh);
            render_bounding_boxes(image, detections, label_to_color);
            const std::string out_name = file.name().substr(0, file.name().rfind(".")) + ".png";
            dlib::save_png(image, output_dir + "/" + out_name);
            std::cerr << file.name() << ": " << detections.size() << " detections\n";
        }
        return EXIT_SUCCESS;
    }

    win.conf_thresh = conf_thresh;
    const std::string out_path = dlib::get_option(parser, "output", "");
    cv::VideoCapture vid_src;
    cv::VideoWriter vid_snk;
    if (parser.option("input"))
    {
        const std::string video_path = parser.option("input").argument();
        cv::VideoCapture file(video_path);
        if (not parser.option("fps"))
            fps = file.get(cv::CAP_PROP_FPS);
        vid_src = file;
        win.mirror = false;
    }
    else
    {
        cv::VideoCapture cap(webcam_idx);
        cap.set(cv::CAP_PROP_FPS, fps);
        vid_src = cap;
        win.mirror = true;
    }
    int width, height;
    {
        cv::Mat cv_tmp;
        vid_src.read(cv_tmp);
        width = cv_tmp.cols;
        height = cv_tmp.rows;
    }
    if (out_width > 0)
    {
        height *= out_width / width;
        width = out_width;
    }
    if (not out_path.empty())
    {
        vid_snk = cv::VideoWriter(
            out_path,
            cv::VideoWriter::fourcc('X', '2', '6', '4'),
            fps,
            cv::Size(width, height));
    }

    dlib::running_stats_decayed<float> rs(10);
    std::cout << std::fixed << std::setprecision(2);
    while (not win.is_closed())
    {
        dlib::matrix<dlib::rgb_pixel> image;
        cv::Mat cv_cap;
        if (!vid_src.read(cv_cap))
        {
            break;
        }
        // convert the BRG opencv image to RGB dlib image
        const dlib::cv_image<dlib::bgr_pixel> tmp(cv_cap);
        if (win.mirror)
            dlib::flip_image_left_right(tmp, image);
        else
            dlib::assign_image(image, tmp);
        win.clear_overlay();
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<detection> detections;
        yolo.detect(image, detections, img_size, win.conf_thresh, nms_thresh);
        const auto t1 = std::chrono::steady_clock::now();
        rs.add(std::chrono::duration_cast<std::chrono::duration<float>>(t1 - t0).count());
        std::cout << "avg fps: " << 1.0f / rs.mean() << '\r' << std::flush;
        if (out_width > 0)
            dlib::resize_image(static_cast<double>(out_width) / image.nc(), image);
        render_bounding_boxes(image, detections, label_to_color);
        win.set_image(image);
        if (not out_path.empty())
        {
            dlib::matrix<dlib::bgr_pixel> bgr_img(height, width);
            dlib::assign_image(bgr_img, image);
            vid_snk.write(dlib::toMat(bgr_img));
        }
    }
    if (not out_path.empty())
        vid_snk.release();

    return EXIT_SUCCESS;
}
catch (const std::exception& e)
{
    std::cout << e.what() << '\n';
    return EXIT_FAILURE;
}
