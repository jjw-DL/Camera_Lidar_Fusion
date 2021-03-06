
#include <iostream>
#include <algorithm>
#include <numeric>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        // pixel coordinates
        pt.x = Y.at<double>(0, 0) / Y.at<double>(2, 0); 
        pt.y = Y.at<double>(1, 0) / Y.at<double>(2, 0); 

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}

/* 
* The show3DObjects() function below can handle different output image sizes, but the text output has been manually tuned to fit the 2000x2000 size. 
* However, you can make this function work for other sizes too.
* For instance, to use a 1000x1000 size, adjusting the text positions by dividing them by 2.
*/
void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;  // y??????????????????
            left = left<x ? left : x; // x??????????????????
            bottom = bottom>y ? bottom : y; // y??????????????????
            right = right>x ? right : x; // x??????????????????

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 0.5, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 0.5, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}


// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    double dist_mean = 0;
    std::vector<cv::DMatch>  kptMatches_roi;

    float shrinkFactor = 0.15;
    cv::Rect smallerBox_c;
    // shrink current bounding box slightly to avoid having too many outlier points around the edges
    smallerBox_c.x = boundingBox.roi.x + shrinkFactor * boundingBox.roi.width / 2.0;
    smallerBox_c.y = boundingBox.roi.y + shrinkFactor * boundingBox.roi.height / 2.0;
    smallerBox_c.width = boundingBox.roi.width * (1 - shrinkFactor);
    smallerBox_c.height = boundingBox.roi.height * (1 - shrinkFactor);

    std::vector<double> euclideanDistance;

    for(auto it = kptMatches.begin(); it!= kptMatches.end();++it)
    {
        int currKptIndex = (*it).trainIdx;
        const auto &currKeyPoint = kptsCurr[currKptIndex];

        if(smallerBox_c.contains(currKeyPoint.pt))
        {
            int prevKptIndex = (*it).queryIdx;
            const auto &prevKeyPoint = kptsPrev[prevKptIndex];

            euclideanDistance.push_back(cv::norm(currKeyPoint.pt - prevKeyPoint.pt));
        }
    }

    int pair_num =  euclideanDistance.size();
    double euclideanDistanceMean = std::accumulate(euclideanDistance.begin(), euclideanDistance.end(), 0.0) / pair_num;

    for(auto it = kptMatches.begin();it != kptMatches.end();it++)
    {
        int currKptIndex = (*it).trainIdx;
        const auto &currKeyPoint = kptsCurr[currKptIndex];

        if(smallerBox_c.contains(currKeyPoint.pt))
        {
            int prevKptIndex = (*it).queryIdx;
            const auto &prevKeyPoint = kptsPrev[prevKptIndex];

            double temp = cv::norm(currKeyPoint.pt - prevKeyPoint.pt);
            double euclideanDistanceMean_Augment = euclideanDistanceMean * 1.3;
            if(temp < euclideanDistanceMean_Augment)
            {
                boundingBox.keypoints.push_back(currKeyPoint);
                boundingBox.kptMatches.push_back(*it);
            }
        }
    }
    std::cout << "mean value: " << euclideanDistanceMean << " Before filtering there are: " << pair_num << " and after filtering, there are " << boundingBox.keypoints.size() << std::endl;
}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{
    vector<double> distRatios;
    for(auto it1 = kptMatches.begin();it1!=kptMatches.end()-1;++it1)
    {
        cv::KeyPoint kpOUterCurr = kptsCurr.at(it1->trainIdx);
        cv::KeyPoint kpOUterPrev = kptsPrev.at(it1->queryIdx);

        for(auto it2 = it1 + 1; it2 !=kptMatches.end();++it2)
        {
            double minDist = 100;
            cv::KeyPoint kpInnerCurr = kptsCurr.at(it2->trainIdx);
            cv::KeyPoint kpInnerPrev = kptsPrev.at(it2->queryIdx);

            double distCurr = cv::norm(kpOUterCurr.pt-kpInnerCurr.pt);
            double distPrev = cv::norm(kpOUterPrev.pt - kpInnerPrev.pt);

            if(distPrev > std::numeric_limits<double>::epsilon() && distCurr >= minDist)
            {
                double distRatio = distCurr / distPrev;
                distRatios.push_back(distRatio);
            }
        }
    }

    if (distRatios.size() == 0)
    {
        TTC = NAN;
        return;
    }

    std::sort(distRatios.begin(), distRatios.end());
    long medIndex = floor(distRatios.size()/2.0);
    double medDistRatio = distRatios.size() % 2 == 0 ? (distRatios[medIndex-1]+distRatios[medIndex])/2:distRatios[medIndex];
    double dT = 1 / frameRate;
    TTC = - dT / (1-medDistRatio);
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC)
{
    int lane_wide = 4;
    //just consider Lidar points within ego lane
    std::vector<float> ppx;
    std::vector<float> pcx;
    for(auto it = lidarPointsPrev.begin(); it != lidarPointsPrev.end() -1; ++it)
    {
        if(abs(it->y) < lane_wide/2) ppx.push_back(it->x);
    }
    for(auto it = lidarPointsCurr.begin(); it != lidarPointsCurr.end() -1; ++it)
    {
        if(abs(it->y) < lane_wide/2) pcx.push_back(it->x);
    }

    float min_px, min_cx;
    int p_size = ppx.size();
    int c_size = pcx.size();
    if(p_size > 0 && c_size > 0)
    {
        for(int i=0; i<p_size; i++)
        {
            min_px += ppx[i];
        }

        for(int j=0; j<c_size; j++)
        {
            min_cx += pcx[j];
        }
    }
    else 
    {
        TTC = NAN;
        return;
    }

    min_px = min_px /p_size;
    min_cx = min_cx /c_size;
    std::cout<<"lidar_min_px:"<<min_px<<std::endl;
    std::cout<<"lidar_min_cx:"<<min_cx<<std::endl;

    float dt = 1/frameRate;
    TTC = min_cx * dt / (min_px - min_cx);
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame)
{
    // NOTE: After calling a cv::DescriptorMatcher::match function, each DMatch
    // contains two keypoint indices, queryIdx and trainIdx, based on the order of image arguments to match.
    // https://docs.opencv.org/4.1.0/db/d39/classcv_1_1DescriptorMatcher.html#a0f046f47b68ec7074391e1e85c750cba
    // prevFrame.keypoints is indexed by queryIdx
    // currFrame.keypoints is indexed by trainIdx
    int p = prevFrame.boundingBoxes.size();
    int c = currFrame.boundingBoxes.size();
    int pt_counts[p][c] ={}; // ????????????[p,c]???????????????
    
    for(auto it = matches.begin();it!=matches.end()-1;++it)
    {
        cv::KeyPoint query = prevFrame.keypoints[it->queryIdx];
        auto query_pt = cv::Point(query.pt.x,query.pt.y);
        bool query_found = false;

        cv::KeyPoint train = currFrame.keypoints[it->trainIdx];
        auto train_pt = cv::Point(train.pt.x,train.pt.y);
        bool train_found = false;

        std::vector<int> query_id,train_id;
        for(int i = 0 ; i < p; i++) // ??????????????????????????????boundingbox??????????????????
        {
            if(prevFrame.boundingBoxes[i].roi.contains(query_pt))
            {
                query_found = true;
                query_id.push_back(i);
            }
        }

        for(int i =0 ; i < c; i++) // ??????????????????????????????boundingbox??????????????????
        {
            if(currFrame.boundingBoxes[i].roi.contains(train_pt))
            {
                train_found = true;
                train_id.push_back(i);
            }
        }

        if(query_found && train_found)
        {
            for(auto id_prev : query_id)
            {
                for(auto id_curr : train_id)
                {
                    pt_counts[id_prev][id_curr] += 1;
                }
            }
                      
        }
    }
    // ?????????????????????i???boxes??????????????????j???boxes???????????????match?????????????????????
    for(int i = 0 ; i < p ;i++)
    {
        int max_count = 0;
        int id_max = 0;
        for(int j = 0; j < c; j++)
        {
            if(pt_counts[i][j] > max_count)
            {
                max_count = pt_counts[i][j];
                id_max = j;
            }
        }
        bbBestMatches[i] = id_max;
    }
}
