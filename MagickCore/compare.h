/*
  Copyright 1999-2014 ImageMagick Studio LLC, a non-profit organization
  dedicated to making software imaging solutions freely available.
  
  You may not use this file except in compliance with the License.
  obtain a copy of the License at
  
    http://www.imagemagick.org/script/license.php
  
  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  MagickCore image compare methods.
*/
#ifndef _MAGICKCORE_COMPARE_H
#define _MAGICKCORE_COMPARE_H

#include "MagickCore/image.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

typedef enum
{
  UndefinedErrorMetric,
  AbsoluteErrorMetric,
  FuzzErrorMetric,
  MeanAbsoluteErrorMetric,
  MeanErrorPerPixelMetric,
  MeanSquaredErrorMetric,
  NormalizedCrossCorrelationErrorMetric,
  PeakAbsoluteErrorMetric,
  PeakSignalToNoiseRatioMetric,
  PerceptualHashErrorMetric,
  RootMeanSquaredErrorMetric
} MetricType;

extern MagickExport double
  *GetImageDistortions(Image *,const Image *,const MetricType,ExceptionInfo *);

extern MagickExport Image
  *CompareImages(Image *,const Image *,const MetricType,double *,
    ExceptionInfo *),
  *SimilarityImage(Image *,const Image *,const MetricType,const double,
    RectangleInfo *,double *,ExceptionInfo *);

extern MagickExport MagickBooleanType
  GetImageDistortion(Image *,const Image *,const MetricType,double *,
    ExceptionInfo *),
  IsImagesEqual(Image *,const Image *,ExceptionInfo *);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
