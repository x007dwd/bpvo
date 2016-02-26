#include "bpvo/photo_error.h"
#include "bpvo/imwarp.h"
#include "bpvo/project_points.h"
#include "bpvo/simd.h"

#include <opencv2/core/core.hpp>
#include <vector>

namespace bpvo {

#define PHOTO_ERROR_WITH_OPENCV 0

#if PHOTO_ERROR_WITH_OPENCV

#include <opencv2/imgproc/imgproc.hpp>


struct PhotoError::Impl
{
  typedef Eigen::Map<const Point, Eigen::Aligned> PointMap;

  void init(const Matrix34& P, const PointVector& X, ValidVector& valid, int rows, int cols)
  {
    int N = X.size();
    resize(N);
    valid.resize(N);

    if(N > 0)
    {
      Eigen::Vector3f p;
      for(int i = 0; i < N; ++i)
      {
        p = P * X[i];
        float w_i = 1.f / p[2];
        _x[i] = w_i * p[0];
        _y[i] = w_i * p[1];

        int xi = static_cast<int>(_x[i]),
            yi = static_cast<int>(_y[i]);

        valid[i] = xi>=0 && xi<cols-1 && yi>=0 && yi<rows-1;
      }

      cv::convertMaps(_x, _y, _map1, _map2, CV_16SC2);
    }
  }

  void run(const float* I0_ptr, const cv::Mat_<float>& I1, float* r_ptr)
  {
    if(_x.empty())
      return;

    cv::Mat dst;
    cv::remap(I1, dst, _map1, _map2, cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));

    auto I1_ptr = dst.ptr<float>();
    for(size_t i = 0; i < _x.size(); ++i)
      r_ptr[i] = I1_ptr[i] - I0_ptr[i];
  }

  void resize(size_t n)
  {
    _x.resize(n);
    _y.resize(n);
  }

  std::vector<float> _x;
  std::vector<float> _y;
  cv::Mat _map1, _map2;
}; // PhotoError

#else

struct PhotoError::Impl
{
  typedef Eigen::Matrix<float,4,1> Vector4;
  typedef typename EigenAlignedContainer<Vector4>::type CoeffsVector;

  void init(const Matrix34& P, const PointVector& X, ValidVector& valid, int rows, int cols)
  {
    resize(X.size());
    valid.resize(X.size());
    _valid_ptr = valid.data();
    _stride = cols;

    if(X.empty())
      return;

    projectPoints(P, X[0].data(), X.size(), ImageSize(rows, cols),
                  valid.data(), _inds.data(), _interp_coeffs[0].data());

  }

  void run(const float* I0_ptr, const cv::Mat_<float>& I1, float* r_ptr) const
  {
    int num_points = _interp_coeffs.size();
    if(num_points == 0)
      return;

    auto I1_ptr = I1.ptr<const float>();
    int i = 0;

    if(simd::isAligned<16>(r_ptr) && simd::isAligned<16>(I0_ptr))
    {
      for(i = 0; i <= num_points - 8; i += 8)
      {
        _mm_store_ps(r_ptr + i,
                     _mm_sub_ps(_mm_setr_ps(
                             this->operator()(I1_ptr, i + 0),
                             this->operator()(I1_ptr, i + 1),
                             this->operator()(I1_ptr, i + 2),
                             this->operator()(I1_ptr, i + 3)), _mm_load_ps(I0_ptr + i)));
        _mm_store_ps(r_ptr + i + 4,
                     _mm_sub_ps(_mm_setr_ps(
                             this->operator()(I1_ptr, i + 4),
                             this->operator()(I1_ptr, i + 5),
                             this->operator()(I1_ptr, i + 6),
                             this->operator()(I1_ptr, i + 7)), _mm_load_ps(I0_ptr + i + 4)));
      }
    } else {
      for(i = 0; i <= num_points - 8; i += 8)
      {
        _mm_storeu_ps(r_ptr + i,
                      _mm_sub_ps(_mm_setr_ps(
                              this->operator()(I1_ptr, i + 0),
                              this->operator()(I1_ptr, i + 1),
                              this->operator()(I1_ptr, i + 2),
                              this->operator()(I1_ptr, i + 3)), _mm_loadu_ps(I0_ptr + i)));
        _mm_storeu_ps(r_ptr + i + 4,
                      _mm_sub_ps(_mm_setr_ps(
                              this->operator()(I1_ptr, i + 4),
                              this->operator()(I1_ptr, i + 5),
                              this->operator()(I1_ptr, i + 6),
                              this->operator()(I1_ptr, i + 7)), _mm_loadu_ps(I0_ptr + i + 4)));
      }
    }


    for( ; i < num_points; ++i)
      r_ptr[i] = this->operator()(I1_ptr, i) - I0_ptr[i];
  }

  void resize(size_t N)
  {
    _interp_coeffs.resize(N);
    _inds.resize(N);
  }

  inline Vector4 load_data(const float* ptr, int i) const
  {
    auto p = ptr + _inds[i];
    return Vector4(*p, *(p + 1), *(p + _stride), *(p + _stride + 1));
  }

  inline float operator()(const float* ptr, int i) const
  {
#if defined(WITH_SIMD)
    return _valid_ptr[i] ? dot_( _mm_load_ps(_interp_coeffs[i].data()),
                                 load_data_simd(ptr + _inds[i]) ) : 0.0f;
#else
    return _valid_ptr[i] ? dot_(_interp_coeffs[i], load_data(ptr, i)) : 0.0f;
#endif
  }

#if defined(WITH_SIMD)
  inline __m128 load_data_simd(const float* p) const
  {
    //
    // NOTE this might segfault for points near the boundary, we should adjust
    // the border/valid mask when selecting points
    //
    return _mm_shuffle_ps(_mm_loadu_ps(p), _mm_loadu_ps(p +  _stride),
                          _MM_SHUFFLE(1,0,1,0));
  }

  inline float dot_(__m128 a, __m128 b) const
  {
    float ret = 0.0f;
#if defined(__SSE4_1__)
    _mm_store_ss(&ret, _mm_dp_ps(a, b, 0xff));
#else
    auto t0 = _mm_mul_ps(a, b),
         t1 = _mm_hadd_ps(t0, t0),
         t2 = _mm_hadd_ps(t1, t1);
    _mm_store_ss(&ret, t2);
#endif
    return ret;
  }

#endif

  inline float dot_(const Vector4& a, const Vector4& b) const
  {
#if defined(__SSE4_1__)
    float ret;
    _mm_store_ss(&ret, _mm_dp_ps(_mm_load_ps(a.data()), _mm_load_ps(b.data()), 0xff));
    return ret;
#else
    // EIGEN uses 2 applications of hadd after mul, dp seems faster if we have sse4
    return a.dot(b);
#endif
  }

  int _stride;
  CoeffsVector _interp_coeffs;
  std::vector<int> _inds;
  const typename ValidVector::value_type* _valid_ptr = nullptr;
}; // PhotoError::Impl

#endif

PhotoError::PhotoError()
  : _impl(new PhotoError::Impl) {}

  PhotoError::~PhotoError() {}

void PhotoError::init(const Matrix34& P, const PointVector& X, ValidVector& valid, int rows, int cols)
{
  _impl->init(P, X, valid, rows, cols);
}

void PhotoError::run(const float* I0_ptr, cv::InputArray I1_, float* r_ptr) const
{
  const cv::Mat_<float>& I1 = (const cv::Mat_<float>&) I1_.getMat();
  _impl->run(I0_ptr, I1, r_ptr);
}

#undef PHOTO_ERROR_WITH_OPENCV

}; // bpvo