/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%             CCCC   OOO   N   N  V   V   OOO   L      V   V  EEEEE           %
%            C      O   O  NN  N  V   V  O   O  L      V   V  E               %
%            C      O   O  N N N  V   V  O   O  L      V   V  EEE             %
%            C      O   O  N  NN   V V   O   O  L       V V   E               %
%             CCCC   OOO   N   N    V     OOO   LLLLL    V    EEEEE           %
%                                                                             %
%                              Convolve An Image                              %
%                                                                             %
%                               Software Design                               %
%                                 John Cristy                                 %
%                                November 2009                                %
%                                                                             %
%                                                                             %
%  Copyright 1999-2010 ImageMagick Studio LLC, a non-profit organization      %
%  dedicated to making software imaging solutions freely available.           %
%                                                                             %
%  You may not use this file except in compliance with the License.  You may  %
%  obtain a copy of the License at                                            %
%                                                                             %
%    http://www.imagemagick.org/script/license.php                            %
%                                                                             %
%  Unless required by applicable law or agreed to in writing, software        %
%  distributed under the License is distributed on an "AS IS" BASIS,          %
%  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   %
%  See the License for the specific language governing permissions and        %
%  limitations under the License.                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
% Convolve an image by executing in concert across heterogeneous platforms
% consisting of CPUs, GPUs, and other processors (in development).
%
*/

/*
  Include declarations.
*/
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <magick/studio.h>
#include <magick/MagickCore.h>

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%   c o n v o l v e I m a g e                                                 %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  convolveImage() convolves an image by utilizing the OpenCL framework to
%  execute the algorithm across heterogeneous platforms consisting of CPUs,
%  GPUs, and other processors.  The format of the convolveImage method is:
%
%      unsigned long convolveImage(Image *images,const int argc,
%        char **argv,ExceptionInfo *exception)
%
%  A description of each parameter follows:
%
%    o image: the address of a structure of type Image.
%
%    o argc: Specifies a pointer to an integer describing the number of
%      elements in the argument vector.
%
%    o argv: Specifies a pointer to a text array containing the command line
%      arguments.
%
%    o exception: return any errors or warnings in this structure.
%
*/

#if defined(MAGICKCORE_OPENCL_SUPPORT)

#if defined(MAGICKCORE_HDRI_SUPPORT)
#define CLOptions "-DMAGICKCORE_HDRI_SUPPORT=1 -DCLQuantum=float " \
  "-DCLPixelType=float4 -DQuantumRange=%.15g -DMagickEpsilon=%.15g"
#define CLPixelPacket  cl_ufloat4
#else
#if (MAGICKCORE_QUANTUM_DEPTH == 8)
#define CLOptions "-DCLQuantum=uchar -DCLPixelType=uchar4 " \
  "-DQuantumRange=%.15g -DMagickEpsilon=%.15g"
#define CLPixelPacket  cl_uchar4
#elif (MAGICKCORE_QUANTUM_DEPTH == 16)
#define CLOptions "-DCLQuantum=ushort -DCLPixelType=ushort4 " \
  "-DQuantumRange=%.15g -DMagickEpsilon=%.15g"
#define CLPixelPacket  cl_ushort4
#elif (MAGICKCORE_QUANTUM_DEPTH == 32)
#define CLOptions "-DCLQuantum=uint -DCLPixelType=uint4 " \
  "-DQuantumRange=%.15g -DMagickEpsilon=%.15g"
#define CLPixelPacket  cl_uint4
#elif (MAGICKCORE_QUANTUM_DEPTH == 32)
#define CLOptions "-DCLQuantum=ulong -DCLPixelType=ulong4 " \
  "-DQuantumRange=%.15g -DMagickEpsilon=%.15g"
#define CLPixelPacket  cl_ulong4
#endif
#endif

typedef struct _CLInfo
{
  cl_context
    context;

  cl_device_id
    *devices;

  cl_command_queue
    command_queue;

  cl_kernel
    kernel;

  cl_program
    program;

  cl_mem
    pixels,
    convolve_pixels;

  cl_ulong
    width,
    height;

  cl_bool
    matte;

  cl_mem
    filter;
} CLInfo;

static char
  *convolve_program =
    "static inline long ClampToCanvas(const long offset,const ulong range)\n"
    "{\n"
    "  if (offset < 0L)\n"
    "    return(0L);\n"
    "  if (offset >= range)\n"
    "    return((long) (range-1L));\n"
    "  return(offset);\n"
    "}\n"
    "\n"
    "static inline CLQuantum ClampToQuantum(const double value)\n"
    "{\n"
    "#if !defined(MAGICKCORE_HDRI_SUPPORT)\n"
    "  if (value < 0.0)\n"
    "    return((CLQuantum) 0);\n"
    "  if (value >= (double) QuantumRange)\n"
    "    return((CLQuantum) QuantumRange);\n"
    "  return((CLQuantum) (value+0.5));\n"
    "#else\n"
    "  return((CLQuantum) value)\n"
    "#endif\n"
    "}\n"
    "\n"
    "__kernel void Convolve(const __global CLPixelType *input,\n"
    "  __constant double *filter,const ulong width,const ulong height,\n"
    "  const bool matte,__global CLPixelType *output)\n"
    "{\n"
    "  const ulong columns = get_global_size(0);\n"
    "  const ulong rows = get_global_size(1);\n"
    "\n"
    "  const long x = get_global_id(0);\n"
    "  const long y = get_global_id(1);\n"
    "\n"
    "  const double scale = (1.0/QuantumRange);\n"
    "  const long mid_width = (width-1)/2;\n"
    "  const long mid_height = (height-1)/2;\n"
    "  double4 sum = { 0.0, 0.0, 0.0, 0.0 };\n"
    "  double gamma = 0.0;\n"
    "  register ulong i = 0;\n"
    "\n"
    "  int method = 0;\n"
    "  if (matte != false)\n"
    "    method=1;\n"
    "  if ((x >= width) && (x < (columns-width-1)) &&\n"
    "      (y >= height) && (y < (rows-height-1)))\n"
    "    {\n"
    "      method=2;\n"
    "      if (matte != false)\n"
    "        method=3;\n"
    "    }\n"
    "  switch (method)\n"
    "  {\n"
    "    case 0:\n"
    "    {\n"
    "      for (long v=(-mid_height); v <= mid_height; v++)\n"
    "      {\n"
    "        for (long u=(-mid_width); u <= mid_width; u++)\n"
    "        {\n"
    "          const long index=ClampToCanvas(y+v,rows)*columns+\n"
    "            ClampToCanvas(x+u,columns);\n"
    "          sum.x+=filter[i]*input[index].x;\n"
    "          sum.y+=filter[i]*input[index].y;\n"
    "          sum.z+=filter[i]*input[index].z;\n"
    "          gamma+=filter[i];\n"
    "          i++;\n"
    "        }\n"
    "      }\n"
    "      break;\n"
    "    }\n"
    "    case 1:\n"
    "    {\n"
    "      for (long v=(-mid_height); v <= mid_height; v++)\n"
    "      {\n"
    "        for (long u=(-mid_width); u <= mid_width; u++)\n"
    "        {\n"
    "          const ulong index=ClampToCanvas(y+v,rows)*columns+\n"
    "            ClampToCanvas(x+u,columns);\n"
    "          const double alpha=scale*(QuantumRange-input[index].w);\n"
    "          sum.x+=alpha*filter[i]*input[index].x;\n"
    "          sum.y+=alpha*filter[i]*input[index].y;\n"
    "          sum.z+=alpha*filter[i]*input[index].z;\n"
    "          sum.w+=filter[i]*input[index].w;\n"
    "          gamma+=alpha*filter[i];\n"
    "          i++;\n"
    "        }\n"
    "      }\n"
    "      break;\n"
    "    }\n"
    "    case 2:\n"
    "    {\n"
    "      for (long v=(-mid_height); v <= mid_height; v++)\n"
    "      {\n"
    "        for (long u=(-mid_width); u <= mid_width; u++)\n"
    "        {\n"
    "          const ulong index=(y+v)*columns+(x+u);\n"
    "          sum.x+=filter[i]*input[index].x;\n"
    "          sum.y+=filter[i]*input[index].y;\n"
    "          sum.z+=filter[i]*input[index].z;\n"
    "          gamma+=filter[i];\n"
    "          i++;\n"
    "        }\n"
    "      }\n"
    "      break;\n"
    "    }\n"
    "    case 3:\n"
    "    {\n"
    "      for (long v=(-mid_height); v <= mid_height; v++)\n"
    "      {\n"
    "        for (long u=(-mid_width); u <= mid_width; u++)\n"
    "        {\n"
    "          const ulong index=(y+v)*columns+(x+u);\n"
    "          const double alpha=scale*(QuantumRange-input[index].w);\n"
    "          sum.x+=alpha*filter[i]*input[index].x;\n"
    "          sum.y+=alpha*filter[i]*input[index].y;\n"
    "          sum.z+=alpha*filter[i]*input[index].z;\n"
    "          sum.w+=filter[i]*input[index].w;\n"
    "          gamma+=alpha*filter[i];\n"
    "          i++;\n"
    "        }\n"
    "      }\n"
    "      break;\n"
    "    }\n"
    "  }\n"
    "  gamma=1.0/(fabs(gamma) <= MagickEpsilon ? 1.0 : gamma);\n"
    "  const ulong index=y*columns+x;\n"
    "  output[index].x=ClampToQuantum(gamma*sum.x);\n"
    "  output[index].y=ClampToQuantum(gamma*sum.y);\n"
    "  output[index].z=ClampToQuantum(gamma*sum.z);\n"
    "  if (matte == false)\n"
    "    output[index].w=input[index].w;\n"
    "  else\n"
    "    output[index].w=ClampToQuantum(sum.w);\n"
    "}\n";

static void OpenCLNotify(const char *message,const void *data,size_t length,
  void *user_context)
{
  ExceptionInfo
    *exception;

  (void) data;
  (void) length;
  exception=(ExceptionInfo *) user_context;
  (void) ThrowMagickException(exception,GetMagickModule(),FilterError,
    "","`%s'",message);
}

static MagickBooleanType BindCLParameters(CLInfo *cl_info,Image *image,
  void *pixels,double *filter,const unsigned long width,
  const unsigned long height,void *convolve_pixels)
{
  cl_int
    status;

  register int
    i;

  size_t
    length;

  /*
    Allocate OpenCL buffers.
  */
  length=image->columns*image->rows;
  cl_info->pixels=clCreateBuffer(cl_info->context,CL_MEM_READ_ONLY |
    CL_MEM_USE_HOST_PTR,length*sizeof(CLPixelPacket),pixels,&status);
  if ((cl_info->pixels == (cl_mem) NULL) || (status != CL_SUCCESS))
    return(MagickFalse);
  length=width*height;
  cl_info->filter=clCreateBuffer(cl_info->context,CL_MEM_READ_ONLY |
    CL_MEM_USE_HOST_PTR,length*sizeof(cl_double),filter,&status);
  if ((cl_info->filter == (cl_mem) NULL) || (status != CL_SUCCESS))
    return(MagickFalse);
  length=image->columns*image->rows;
  cl_info->convolve_pixels=clCreateBuffer(cl_info->context,CL_MEM_WRITE_ONLY |
    CL_MEM_USE_HOST_PTR,length*sizeof(CLPixelPacket),convolve_pixels,&status);
  if ((cl_info->convolve_pixels == (cl_mem) NULL) || (status != CL_SUCCESS))
    return(MagickFalse);
  /*
    Bind OpenCL buffers.
  */
  i=0;
  status=clSetKernelArg(cl_info->kernel,i++,sizeof(cl_mem),(void *)
    &cl_info->pixels);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  status=clSetKernelArg(cl_info->kernel,i++,sizeof(cl_mem),(void *)
    &cl_info->filter);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  cl_info->width=(cl_ulong) width;
  status=clSetKernelArg(cl_info->kernel,i++,sizeof(cl_ulong),(void *)
    &cl_info->width);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  cl_info->height=(cl_ulong) height;
  status=clSetKernelArg(cl_info->kernel,i++,sizeof(cl_ulong),(void *)
    &cl_info->height);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  cl_info->matte=(cl_bool) image->matte;
  status=clSetKernelArg(cl_info->kernel,i++,sizeof(cl_bool),(void *)
    &cl_info->matte);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  status=clSetKernelArg(cl_info->kernel,i++,sizeof(cl_mem),(void *)
    &cl_info->convolve_pixels);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  status=clFinish(cl_info->command_queue);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  return(MagickTrue);
}

static void DestroyCLBuffers(CLInfo *cl_info)
{
  cl_int
    status;

  if (cl_info->convolve_pixels != (cl_mem) NULL)
    status=clReleaseMemObject(cl_info->convolve_pixels);
  if (cl_info->pixels != (cl_mem) NULL)
    status=clReleaseMemObject(cl_info->pixels);
  if (cl_info->filter != (cl_mem) NULL)
    status=clReleaseMemObject(cl_info->filter);
}

static CLInfo *DestroyCLInfo(CLInfo *cl_info)
{
  cl_int
    status;

  if (cl_info->kernel != (cl_kernel) NULL)
    status=clReleaseKernel(cl_info->kernel);
  if (cl_info->program != (cl_program) NULL)
    status=clReleaseProgram(cl_info->program);
  if (cl_info->command_queue != (cl_command_queue) NULL)
    status=clReleaseCommandQueue(cl_info->command_queue);
  if (cl_info->context != (cl_context) NULL)
    status=clReleaseContext(cl_info->context);
  cl_info=(CLInfo *) RelinquishMagickMemory(cl_info);
  return(cl_info);
}

static MagickBooleanType EnqueueKernel(CLInfo *cl_info,Image *image,
  void *pixels,double *filter,const unsigned long width,
  const unsigned long height,void *convolve_pixels)
{
  cl_int
    status;

  size_t
    global_work_size[2],
    length;

  length=image->columns*image->rows;
  status=clEnqueueWriteBuffer(cl_info->command_queue,cl_info->pixels,CL_TRUE,0,
    length*sizeof(CLPixelPacket),pixels,0,NULL,NULL);
  length=width*height;
  status=clEnqueueWriteBuffer(cl_info->command_queue,cl_info->filter,CL_TRUE,0,
    length*sizeof(cl_double),filter,0,NULL,NULL);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  global_work_size[0]=image->columns;
  global_work_size[1]=image->rows;
  status=clEnqueueNDRangeKernel(cl_info->command_queue,cl_info->kernel,2,NULL,
    global_work_size,NULL,0,NULL,NULL);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  length=image->columns*image->rows;
  status=clEnqueueReadBuffer(cl_info->command_queue,cl_info->convolve_pixels,
    CL_TRUE,0,length*sizeof(CLPixelPacket),convolve_pixels,0,NULL,NULL);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  status=clFinish(cl_info->command_queue);
  if (status != CL_SUCCESS)
    return(MagickFalse);
  return(MagickTrue);
}

static CLInfo *GetCLInfo(Image *image,const char *name,const char *source,
  ExceptionInfo *exception)
{
  char
    options[MaxTextExtent];

  cl_int
    status;

  CLInfo
    *cl_info;

  size_t
    length,
    lengths[] = { strlen(source) };

  /*
    Create OpenCL info.
  */
  cl_info=(CLInfo *) AcquireAlignedMemory(1,sizeof(*cl_info));
  if (cl_info == (CLInfo *) NULL)
    {
      (void) ThrowMagickException(exception,GetMagickModule(),
        ResourceLimitError,"MemoryAllocationFailed","`%s'",image->filename);
      return((CLInfo *) NULL);
    }
  (void) ResetMagickMemory(cl_info,0,sizeof(*cl_info));
  /*
    Create OpenCL context.
  */
  cl_info->context=clCreateContextFromType((cl_context_properties *) NULL,
    CL_DEVICE_TYPE_GPU,OpenCLNotify,exception,&status);
  if ((cl_info->context == (cl_context) NULL) || (status != CL_SUCCESS))
    cl_info->context=clCreateContextFromType((cl_context_properties *) NULL,
      CL_DEVICE_TYPE_CPU,OpenCLNotify,exception,&status);
  if ((cl_info->context == (cl_context) NULL) || (status != CL_SUCCESS))
    cl_info->context=clCreateContextFromType((cl_context_properties *) NULL,
      CL_DEVICE_TYPE_DEFAULT,OpenCLNotify,exception,&status);
  if ((cl_info->context == (cl_context) NULL) || (status != CL_SUCCESS))
    {
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  /*
    Detect OpenCL devices.
  */
  status=clGetContextInfo(cl_info->context,CL_CONTEXT_DEVICES,0,NULL,&length);
  if ((status != CL_SUCCESS) || (length == 0))
    {
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  cl_info->devices=(cl_device_id *) AcquireMagickMemory(length);
  if (cl_info->devices == (cl_device_id *) NULL)
    {
      (void) ThrowMagickException(exception,GetMagickModule(),
        ResourceLimitError,"MemoryAllocationFailed","`%s'",image->filename);
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  status=clGetContextInfo(cl_info->context,CL_CONTEXT_DEVICES,length,
    cl_info->devices,NULL);
  if (status != CL_SUCCESS)
    {
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  /*
    Create OpenCL command queue.
  */
  cl_info->command_queue=clCreateCommandQueue(cl_info->context,
    cl_info->devices[0],0,&status);
  if ((cl_info->command_queue == (cl_command_queue) NULL) ||
      (status != CL_SUCCESS))
    {
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  /*
    Build OpenCL program.
  */
  cl_info->program=clCreateProgramWithSource(cl_info->context,1,&source,
    lengths,&status);
  if ((cl_info->program == (cl_program) NULL) || (status != CL_SUCCESS))
    {
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  (void) FormatMagickString(options,MaxTextExtent,CLOptions,(double)
    QuantumRange,MagickEpsilon);
  status=clBuildProgram(cl_info->program,1,cl_info->devices,options,NULL,NULL);
  if ((cl_info->program == (cl_program) NULL) || (status != CL_SUCCESS))
    {
      char
        *log;

      status=clGetProgramBuildInfo(cl_info->program,cl_info->devices[0],
        CL_PROGRAM_BUILD_LOG,0,NULL,&length);
      log=(char *) AcquireMagickMemory(length);
      if (log == (char *) NULL)
        {
          DestroyCLInfo(cl_info);
          return((CLInfo *) NULL);
        }
      status=clGetProgramBuildInfo(cl_info->program,cl_info->devices[0],
        CL_PROGRAM_BUILD_LOG,length,log,&length);
      (void) ThrowMagickException(exception,GetMagickModule(),FilterError,
        "failed to build OpenCL program","`%s' (%s)",image->filename,log);
      log=DestroyString(log);
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  /*
    Get a kernel object.
  */
  cl_info->kernel=clCreateKernel(cl_info->program,name,&status);
  if ((cl_info->kernel == (cl_kernel) NULL) || (status != CL_SUCCESS))
    {
      DestroyCLInfo(cl_info);
      return((CLInfo *) NULL);
    }
  return(cl_info);
}

#endif

ModuleExport unsigned long convolveImage(Image **images,const int argc,
  const char **argv,ExceptionInfo *exception)
{
  assert(images != (Image **) NULL);
  assert(*images != (Image *) NULL);
  assert((*images)->signature == MagickSignature);
#if !defined(MAGICKCORE_OPENCL_SUPPORT)
  (void) argc;
  (void) argv;
  (void) ThrowMagickException(exception,GetMagickModule(),MissingDelegateError,
    "DelegateLibrarySupportNotBuiltIn","`%s' (OpenCL)",(*images)->filename);
#else
  {
    MagickKernel
      *kernel;

    Image
      *image;

    MagickBooleanType
      status;

    CLInfo
      *cl_info;

    if (argc < 1)
      return(MagickImageFilterSignature);
    /*
      Convolve image.
    */
    kernel=AcquireKernelFromString(argv[0]);
    if (kernel == (MagickKernel *) NULL)
      (void) ThrowMagickException(exception,GetMagickModule(),
        ResourceLimitError,"MemoryAllocationFailed","`%s'",(*images)->filename);
    cl_info=GetCLInfo(*images,"Convolve",convolve_program,exception);
    if (cl_info == (CLInfo *) NULL)
      {
        kernel=DestroyKernel(kernel);
        return(MagickImageFilterSignature);
      }
    image=(*images);
    for ( ; image != (Image *) NULL; image=GetNextImageInList(image))
    {
      Image
        *convolve_image;

      MagickSizeType
        length;

      void
        *convolve_pixels,
        *pixels;

      if (SetImageStorageClass(image,DirectClass) == MagickFalse)
        continue;
      pixels=GetPixelCachePixels(image,&length,exception);
      if (pixels == (void *) NULL)
        {
          (void) ThrowMagickException(exception,GetMagickModule(),CacheError,
            "UnableToReadPixelCache","`%s'",image->filename);
          continue;
        }
      convolve_image=CloneImage(image,image->columns,image->rows,MagickTrue,
        exception);
      convolve_pixels=GetPixelCachePixels(convolve_image,&length,exception);
      if (convolve_pixels == (void *) NULL)
        {
          (void) ThrowMagickException(exception,GetMagickModule(),CacheError,
            "UnableToReadPixelCache","`%s'",image->filename);
          convolve_image=DestroyImage(convolve_image);
          continue;
        }
      status=BindCLParameters(cl_info,image,pixels,kernel->values,kernel->width,
        kernel->height,convolve_pixels);
      if (status != MagickFalse)
        status=EnqueueKernel(cl_info,image,pixels,kernel->values,kernel->width,
          kernel->height,convolve_pixels);
      if (status != MagickFalse)
        (void) CopyMagickMemory(pixels,convolve_pixels,length);
      DestroyCLBuffers(cl_info);
      convolve_image=DestroyImage(convolve_image);
    }
    kernel=DestroyKernel(kernel);
    cl_info=DestroyCLInfo(cl_info);
  }
#endif
  return(MagickImageFilterSignature);
}
