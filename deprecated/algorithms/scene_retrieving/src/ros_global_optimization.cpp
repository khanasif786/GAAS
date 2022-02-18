//receive ros msg:1.image_l,image_r   2.position.
//output:scene struct.
#include <memory>

#include <Eigen/Core>
#include <Eigen/Dense>

#include <opencv2/opencv.hpp>
#include "scene_retrieve.h"
#include <iostream>
#include <thread>
#include <ros/ros.h>
#include <std_msgs/String.h>
#include <roseus/StringStamped.h>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/core/persistence.hpp>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <ros/duration.h>

#include <image_process.h>
#include "global_frame_synchronizer.h" //同步信息用.



using namespace std;

ros::Publisher Pub;


template<typename Out>
void split(const std::string &s, char delim, Out result) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        *(result++) = item;
    }
}
std::vector<std::string> __split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, std::back_inserter(elems));
    return elems;
}

std::vector<double> __split_comma_(const std::string input_str)
{
    std::vector<double> elems;
    std::vector<string> str_d_ = __split(input_str,',');
    LOG(INFO)<<"in __split_comma_,input:"<<input_str<<endl;
    for(int i = 0;i<str_d_.size();i++)
    {
        elems.push_back(stod(str_d_[i]) );
    }
    return elems;
}

GlobalFrameSyncManager* pGlobalFrameSync;
SceneRetriever* pSceneRetriever;


void GOG_INFO_CALLBACK(const roseus::StringStamped & msg_gog) // here we use roseus::StringStamped.
{
    auto timestamp = msg_gog.header.stamp;
    string content(msg_gog.data);
    // content contains:
    // 1.对应stamp的图像在gog_frame中的id.
    
    // 2.对应stamp的图像在gog中的R,t.//可选.
   

    //step<1> split content and do syntax analysis.
    std::vector<std::string> parts = __split(content,'|');
    std::string id_ = parts[0];
    std::string quat_ = parts[1];
    std::string translation_ = parts[2];
    LOG(INFO)<<"Received GOG msg,id:"<<id_<<",quat:"<<quat_<<",translation:"<<translation_<<endl;
    int id = stoi(id_);
    auto q_temp = __split_comma_(quat_);
    Quaterniond quat(q_temp[0],q_temp[1],q_temp[2],q_temp[3]);
    auto t_temp = __split_comma_(translation_);
    Vector3d translation(t_temp[0],t_temp[1],t_temp[2]);

    pGlobalFrameSync->GOG_msg_callback(msg_gog,id,pSceneRetriever->getScene().getCurrentIndex());
}


void StereoImageCallback(const sensor_msgs::ImageConstPtr& msgLeft,const sensor_msgs::ImageConstPtr& msgRight)
{
    cv::Mat curLeftImage, curRightImage;
    try
    {
        curLeftImage = cv_bridge::toCvShare(msgLeft)->image;
        curRightImage = cv_bridge::toCvShare(msgRight)->image;
    }
    catch(cv_bridge::Exception &e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        LOG(ERROR)<<"Exception in ros_global_optimization StereoImageCallback()!"<<endl;
        return;
    }

    if (curLeftImage.empty() && curRightImage.empty())
    {
        LOG(WARNING)<<"Empty image got in ros_global_optimization StereoImageCallback()."<<endl;
        return;
    }
    pGlobalFrameSync->Image_msg_callback(msgLeft,msgRight);
    
/*
    cv::Mat RT_mat, Q_mat;
    bool match_success;
    int inliers = pSceneRetriever->retrieveSceneFromStereoImage(curLeftImage, curRightImage, Q_mat, RT_mat, match_success);

    if(match_success)
    {
        cout<<" Match success! RT mat is: \n"<<RT_mat<<endl;
        std_msgs::String str;
        stringstream ss;
        std::string str_content;
        ss<<"Frame id:"<<0<<","<<1<<";RT:"<<RT_mat;
        ss>>str_content;
        str.data = str_content.c_str();
        Pub.publish(str);
    }
    else
    {
        return;
    }
*/
}


int main_of_backend_thread()
{
    std::cout << "Starting backend thread."<<endl;
    while(true)
    {
        std::cout << "In backend thread:"<<endl;
        bool match_success_out = false;
        cv::Mat camera_Q_mat;
        cv::Mat RT_mat_out;
        auto ret_val = pGlobalFrameSync->checkLoopTaskQueue(match_success_out,camera_Q_mat,RT_mat_out);
        if(match_success_out)
        {
            LOG(INFO)<<"Match success in backend thread,id:"<<std::get<0>(ret_val)<<","<<std::get<1>(ret_val)<<".RT_mat:"<<RT_mat_out<<endl;
            //cout<<" Match success! RT mat is: \n"<<RT_mat<<endl;
            std_msgs::String str;
            stringstream ss;
            std::string str_content;
            ss<<"Frame id:"<<0<<","<<1<<";RT:"<<RT_mat_out;
            ss>>str_content;
            str.data = str_content.c_str();
            Pub.publish(str);
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    std::cout << "Exiting concurrent thread."<<endl;
}



int main(int argc,char** argv)
{

    cout<<"Usage: ros_global_optimization [CONFIG_FILE_PATH]"<<endl;
    if (argc!=2)
    {
        cout <<"Please check input format."<<endl;
        exit(-1);
    }
    std::string config_file_path(argv[1]);
    cv::FileStorage fSettings(config_file_path,cv::FileStorage::READ);
    cv::Mat Q_mat;
    fSettings["Q_mat"] >> Q_mat;
    if (Q_mat.empty())
    {
        cout<<"Q mat empty, exit."<<endl;
        return -1;
    }
    std::string voc_file_path(fSettings["VOC_FILE_PATH"]);
    std::string scene_path;
    //std::string scene_path(argv[1]), voc_file_path(argv[2]) , l_img_path(argv[3]), r_img_path(argv[4]), Q_mat_path(argv[5]);

    cv::Mat RT_mat = (cv::Mat_<float >(4,4) << 1, 0, 0, 0,
            0, 1, 0, 1,
            0, 0, 1, 0,
            0, 0, 0, 1);

    //ros init
    ros::init(argc, argv, "scene_retrieve");
    ros::NodeHandle nh;

    pGlobalFrameSync = new GlobalFrameSyncManager(pSceneRetriever);


    Pub = nh.advertise<std_msgs::String>("/gaas/ros_gog_scene_retrieving",10);
    //std::shared_ptr<SceneRetriever> pSceneRetrieve(new SceneRetriever(voc_file_path));//, scene_path));
    //std::shared_ptr<SceneRetriever> pSceneRetrieve(new DynamicalSceneRetriever(voc_file_path));//先试试修改原来的类,不加入新的.
    //pSceneRetriever = pSceneRetrieve;
    pSceneRetriever = new SceneRetriever(voc_file_path);


    message_filters::Subscriber<sensor_msgs::Image> left_sub(nh, "/gi/simulation/left/image_raw", 10);
    message_filters::Subscriber<sensor_msgs::Image> right_sub(nh, "/gi/simulation/right/image_raw", 10);
    message_filters::Subscriber<std_msgs::String> global_optimization_graph_sub(nh,"/gi/global_optimization/optimization_result",10);

    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image> sync_pol;
    message_filters::Synchronizer<sync_pol> sync(sync_pol(10), left_sub, right_sub);
    sync.setMaxIntervalDuration(ros::Duration(0.01));
    sync.registerCallback(boost::bind(StereoImageCallback, _1, _2));
    std::thread backend_thread(main_of_backend_thread);
    ros::spin();
    return 0;
}




