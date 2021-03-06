/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2012 The MITRE Corporation                                      *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <QFutureSynchronizer>
#include <QtConcurrentRun>
#include <openbr/openbr_plugin.h>

#include "openbr/core/common.h"
#include "openbr/core/opencvutils.h"

using namespace cv;

namespace br
{

/*!
 * \ingroup transforms
 * \brief Approximate floats as uchar.
 * \author Josh Klontz \cite jklontz
 */
class QuantizeTransform : public Transform
{
    Q_OBJECT
    Q_PROPERTY(float a READ get_a WRITE set_a RESET reset_a)
    Q_PROPERTY(float b READ get_b WRITE set_b RESET reset_b)
    BR_PROPERTY(float, a, 1)
    BR_PROPERTY(float, b, 0)

    void train(const TemplateList &data)
    {
        double minVal, maxVal;
        minMaxLoc(OpenCVUtils::toMat(data.data()), &minVal, &maxVal);
        a = 255.0/(maxVal-minVal);
        b = -a*minVal;
    }

    void project(const Template &src, Template &dst) const
    {
        src.m().convertTo(dst, CV_8U, a, b);
    }
};

BR_REGISTER(Transform, QuantizeTransform)

/*!
 * \ingroup transforms
 * \brief Approximate floats as signed bit.
 * \author Josh Klontz \cite jklontz
 */
class BinarizeTransform : public UntrainableTransform
{
    Q_OBJECT

    void project(const Template &src, Template &dst) const
    {
        const Mat &m = src;
        if ((m.cols % 8 != 0) || (m.type() != CV_32FC1))
            qFatal("Expected CV_32FC1 matrix with a multiple of 8 columns.");
        Mat n(m.rows, m.cols/8, CV_8UC1);
        for (int i=0; i<m.rows; i++)
            for (int j=0; j<m.cols-7; j+=8)
                n.at<uchar>(i,j) = ((m.at<float>(i,j+0) > 0) << 0) +
                                   ((m.at<float>(i,j+1) > 0) << 1) +
                                   ((m.at<float>(i,j+2) > 0) << 2) +
                                   ((m.at<float>(i,j+3) > 0) << 3) +
                                   ((m.at<float>(i,j+4) > 0) << 4) +
                                   ((m.at<float>(i,j+5) > 0) << 5) +
                                   ((m.at<float>(i,j+6) > 0) << 6) +
                                   ((m.at<float>(i,j+7) > 0) << 7);
        dst = n;
    }
};

BR_REGISTER(Transform, BinarizeTransform)

/*!
 * \ingroup transforms
 * \brief Compress two uchar into one uchar.
 * \author Josh Klontz \cite jklontz
 */
class PackTransform : public UntrainableTransform
{
    Q_OBJECT

    void project(const Template &src, Template &dst) const
    {
        const Mat &m = src;
        if ((m.cols % 2 != 0) || (m.type() != CV_8UC1))
            qFatal("Invalid template format.");

        Mat n(m.rows, m.cols/2, CV_8UC1);
        for (int i=0; i<m.rows; i++)
            for (int j=0; j<m.cols/2; j++)
                n.at<uchar>(i,j) = ((m.at<uchar>(i,2*j+0) >> 4) << 4) +
                                   ((m.at<uchar>(i,2*j+1) >> 4) << 0);
        dst = n;
    }
};

BR_REGISTER(Transform, PackTransform)

QVector<Mat> ProductQuantizationLUTs;

/*!
 * \ingroup distances
 * \brief Distance in a product quantized space \cite jegou11
 * \author Josh Klontz
 */
class ProductQuantizationDistance : public Distance
{
    Q_OBJECT
    Q_PROPERTY(bool bayesian READ get_bayesian WRITE set_bayesian RESET reset_bayesian STORED false)
    BR_PROPERTY(bool, bayesian, false)

    float compare(const Template &a, const Template &b) const
    {
        float distance = 0;
        for (int i=0; i<a.size(); i++) {
            const int elements = a[i].total();
            const uchar *aData = a[i].data;
            const uchar *bData = b[i].data;
            const float *lut = (const float*)ProductQuantizationLUTs[i].data;
            for (int j=0; j<elements; j++)
                 distance += lut[j*256*256 + aData[j]*256+bData[j]];
        }
        if (!bayesian) distance = -log(distance+1);
        return distance;
    }
};

BR_REGISTER(Distance, ProductQuantizationDistance)

/*!
 * \ingroup transforms
 * \brief Product quantization \cite jegou11
 * \author Josh Klontz \cite jklontz
 */
class ProductQuantizationTransform : public Transform
{
    Q_OBJECT
    Q_PROPERTY(int n READ get_n WRITE set_n RESET reset_n STORED false)
    Q_PROPERTY(br::Distance *distance READ get_distance WRITE set_distance RESET reset_distance STORED false)
    Q_PROPERTY(bool bayesian READ get_bayesian WRITE set_bayesian RESET reset_bayesian STORED false)
    BR_PROPERTY(int, n, 2)
    BR_PROPERTY(br::Distance*, distance, Distance::make("L2", this))
    BR_PROPERTY(bool, bayesian, false)

    int index;
    QList<Mat> centers;

public:
    ProductQuantizationTransform()
    {
        index = ProductQuantizationLUTs.size();
        ProductQuantizationLUTs.append(Mat());
    }

private:
    void _train(const Mat &data, const QList<int> &labels, Mat *lut, Mat *center)
    {
        Mat clusterLabels;
        kmeans(data, 256, clusterLabels, TermCriteria(TermCriteria::MAX_ITER, 10, 0), 3, KMEANS_PP_CENTERS, *center);

        for (int j=0; j<256; j++)
            for (int k=0; k<256; k++)
                lut->at<float>(0,j*256+k) = distance->compare(center->row(j), center->row(k));

        if (!bayesian) return;

        QList<int> indicies = OpenCVUtils::matrixToVector<int>(clusterLabels);
        QVector<float> genuineScores; genuineScores.reserve(data.rows);
        QVector<float> impostorScores; impostorScores.reserve(data.rows*data.rows/2);
        for (int i=0; i<indicies.size(); i++)
            for (int j=i+1; j<indicies.size(); j++) {
                const float score = lut->at<float>(0, indicies[i]*256+indicies[j]);
                if (labels[i] == labels[j]) genuineScores.append(score);
                else                        impostorScores.append(score);
            }
        genuineScores = Common::Downsample(genuineScores, 256);
        impostorScores = Common::Downsample(impostorScores, 256);

        double hGenuine = Common::KernelDensityBandwidth(genuineScores);
        double hImpostor = Common::KernelDensityBandwidth(impostorScores);

        for (int j=0; j<256; j++)
            for (int k=0; k<256; k++)
                lut->at<float>(0,j*256+k) = log(Common::KernelDensityEstimation(genuineScores, lut->at<float>(0,j*256+k), hGenuine) /
                                                Common::KernelDensityEstimation(impostorScores, lut->at<float>(0,j*256+k), hImpostor));
    }

    void train(const TemplateList &src)
    {
        Mat data = OpenCVUtils::toMat(src.data());
        if (data.cols % n != 0) qFatal("Expected dimensionality to be divisible by n.");
        const QList<int> labels = src.labels<int>();

        Mat &lut = ProductQuantizationLUTs[index];
        lut = Mat(data.cols/n, 256*256, CV_32FC1);

        QList<Mat> subdata, subluts;
        for (int i=0; i<lut.rows; i++) {
            centers.append(Mat());
            subdata.append(data.colRange(i*n,(i+1)*n));
            subluts.append(lut.row(i));
        }

        QFutureSynchronizer<void> futures;
        for (int i=0; i<lut.rows; i++) {
            if (Globals->parallelism) futures.addFuture(QtConcurrent::run(this, &ProductQuantizationTransform::_train, subdata[i], labels, &subluts[i], &centers[i]));
            else                                                                                               _train (subdata[i], labels, &subluts[i], &centers[i]);
        }
        futures.waitForFinished();
    }

    int getIndex(const Mat &m, const Mat &center) const
    {
        int bestIndex = 0;
        double bestDistance = std::numeric_limits<double>::max();
        for (int j=0; j<256; j++) {
            double distance = norm(m, center.row(j), NORM_L2);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = j;
            }
        }
        return bestIndex;
    }

    void project(const Template &src, Template &dst) const
    {
        Mat m = src.m().reshape(1, 1);
        dst = Mat(1, m.cols/n, CV_8UC1);
        for (int i=0; i<dst.m().cols; i++)
            dst.m().at<uchar>(0,i) = getIndex(m.colRange(i*n, (i+1)*n), centers[i]);
    }

    void store(QDataStream &stream) const
    {
        stream << centers << ProductQuantizationLUTs[index];
    }

    void load(QDataStream &stream)
    {
        stream >> centers >> ProductQuantizationLUTs[index];
    }
};

BR_REGISTER(Transform, ProductQuantizationTransform)

} // namespace br

#include "quantize.moc"
