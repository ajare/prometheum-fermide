#pragma once

#include <cassert>
#include <cstdlib>
#include <cmath>


namespace core
{

	class Vector2
	{
	public:

		float x, y;

		inline Vector2() :
			x(0),
			y(0)
		{
		}

		inline Vector2(Vector2 const& vec) :
			x(vec.x),
			y(vec.y)
		{
		}

		inline Vector2(float _x, float _y) :
			x(_x),
			y(_y)
		{
		}

		// Operators
		inline Vector2 operator =(Vector2 const& vec)
		{
			x = vec.x;
			y = vec.y;
			return *this;
		}

		inline bool operator ==(Vector2 const& vec) const
		{
			return(x == vec.x && y == vec.y);
		}

		inline bool operator !=(Vector2 const& vec) const
		{
			return(x != vec.x || y != vec.y);
		}

		inline Vector2 operator +(Vector2 const& vec) const
		{
			return Vector2(x + vec.x, y + vec.y);
		}

		inline Vector2 operator -(Vector2 const& vec) const
		{
			return Vector2(x - vec.x, y - vec.y);
		}

		inline Vector2 operator +(float value) const
		{
			return Vector2(x + value, y + value);
		}

		inline Vector2 operator -(float value) const
		{
			return Vector2(x - value, y - value);
		}

		inline Vector2 operator *(Vector2 const& vec) const
		{
			return Vector2(x * vec.x, y * vec.y);
		}

		inline Vector2 operator *(float scalar) const
		{
			return Vector2(x * scalar, y * scalar);
		}

		inline Vector2 operator /(Vector2 const& vec) const
		{
			return Vector2(x / vec.x, y / vec.y);
		}

		inline Vector2 operator /(float  scalar) const
		{
			return Vector2(x / scalar, y / scalar);
		}

		inline Vector2 operator /(int scalar) const
		{
			return Vector2(x / scalar, y / scalar);
		}

		inline Vector2 operator /(unsigned int scalar) const
		{
			return Vector2(x / scalar, y / scalar);
		}

		inline const Vector2& operator +() const
		{
			return *this;
		}

		inline Vector2 operator -() const
		{
			return Vector2(-x, -y);
		}

		inline Vector2 operator +=(Vector2 const& vec)
		{
			x += vec.x;
			y += vec.y;

			return *this;
		}

		inline Vector2 operator +=(float scalar)
		{
			x += scalar;
			y += scalar;

			return *this;
		}

		inline Vector2 operator -=(Vector2 const& vec)
		{
			x -= vec.x;
			y -= vec.y;

			return *this;
		}

		inline Vector2 operator -=(float  scalar)
		{
			x -= scalar;
			y -= scalar;

			return *this;
		}

		inline Vector2 operator *=(Vector2 const& vec)
		{
			x *= vec.x;
			y *= vec.y;

			return *this;
		}

		inline Vector2 operator *=(float  scalar)
		{
			x *= scalar;
			y *= scalar;

			return *this;
		}

		inline Vector2 operator /=(Vector2 const& vec)
		{
			x /= vec.x;
			y /= vec.y;

			return *this;
		}

		inline Vector2 operator /=(float  scalar)
		{
			assert(scalar != 0.0);

			float  inv = 1.0f / scalar;
			x *= inv;
			y *= inv;

			return *this;
		}

		inline Vector2 operator /=(int scalar)
		{
			assert(scalar != 0.0);

			float  inv = 1.0f / scalar;
			x *= inv;
			y *= inv;

			return *this;
		}

		inline Vector2 operator /=(unsigned int scalar)
		{
			assert(scalar != 0.0);

			float  inv = 1.0f / scalar;
			x *= inv;
			y *= inv;

			return *this;
		}

		inline void set(float _x, float _y)
		{
			x = _x;
			y = _y;
		}

		inline void set(int _x, int _y)
		{
			x = (float)_x;
			y = (float)_y;
		}

		inline float length() const
		{
			return sqrt(x * x + y * y);
		}

		inline float lengthSq() const
		{
			return x * x + y * y;
		}

		inline float lengthSquared() const
		{
			return lengthSq();
		}

		inline float distanceTo(Vector2 const& vec) const
		{
			return sqrt((vec.x - x) * (vec.x - x) + (vec.y - y) * (vec.y - y));
		}

		inline float distanceToSq(Vector2 const& vec) const
		{
			float  dist = (vec.x - x) * (vec.x - x) + (vec.y - y) * (vec.y - y);
			return dist;
		}

		inline Vector2 directionTo(Vector2 const& vec) const
		{
			return (vec - *this).normalisedCopy();
		}

		inline float distanceToLine(Vector2 const& v0, Vector2 const& v1) const
		{
			float l2 = v0.distanceToSq(v1);

			if (l2 == 0.0f)
			{
				return distanceTo(v0);
			}
			else
			{
				float t = (*this - v0).dot(v1 - v0) / l2;

				if (t < 0.0f)
				{
					return distanceTo(v0);
				}
				else if (t > 1.0f)
				{
					return distanceTo(v1);
				}
				else
				{
					return distanceTo(v0.lerp(v1, t));
				}
			}
		}

		inline float distanceToLineSq(Vector2 const& v0, Vector2 const& v1) const
		{
			float l2 = v0.distanceToSq(v1);

			if (l2 == 0.0f)
			{
				return distanceToSq(v0);
			}
			else
			{
				float t = (*this - v0).dot(v1 - v0) / l2;

				if (t < 0.0f)
				{
					return distanceToSq(v0);
				}
				else if (t > 1.0f)
				{
					return distanceToSq(v1);
				}
				else
				{
					return distanceToSq(v0.lerp(v1, t));
				}
			}
		}

		inline float distanceToRay(Vector2 const& rayOrigin, Vector2 const& rayDir) const
		{
			Vector2 d = *this - rayOrigin;
			float rayParam = rayDir.dot(d);

			Vector2 closestPoint = rayParam > 0 ? rayOrigin + rayDir * rayParam : rayOrigin;
			return rayDir.perpendicular().dot(*this - closestPoint);
		}

		inline Vector2 closestPointOnLine(Vector2 const& v0, Vector2 const& v1) const
		{
			Vector2 tv0 = *this - v0;
			Vector2 dv = v1 - v0;

			float dvSq = dv.x * dv.x + dv.y * dv.y;
			float dot = tv0.dot(dv);

			float t = dot / dvSq;
			if (t < 0.0f)
			{
				return v0;
			}
			else if (t > 1.0f)
			{
				return v1;
			}

			return v0.lerp(v1, t);
		}

		inline float distanceToCapsule(Vector2 const& v0, Vector2 const& v1, float radius) const
		{
			Vector2 pv0(x - v0.x, y - v0.y);
			Vector2 pv1(v1.x - v0.x, v1.y - v0.y);

			// This tests the distance to the border.  Negative values mean the point is inside.
			float d = pv0.dot(pv1) / pv1.dot(pv1);
			float h = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
			return pv0.distanceTo(pv1 * h) - radius;
		}

		inline bool withinEpsilon(Vector2 const& vec) const
		{
			return fabs(x - vec.x) < 0.001f && fabs(y - vec.y) < 0.001f;
		}

		inline float dot(Vector2 const& vec) const
		{
			return x * vec.x + y * vec.y;
		}

		inline double normalise()
		{
			double len = sqrt(x * x + y * y);
			if (len > 1e-08)
			{
				double invLen = 1.0 / len;
				x *= (float)invLen;
				y *= (float)invLen;
			}

			return len;
		}

		inline Vector2 normalisedCopy() const
		{
			Vector2 vec = *this;
			vec.normalise();
			return vec;
		}

		inline Vector2 lerp(Vector2 const& vec, float  t) const
		{
			return Vector2(x + (vec.x - x) * t, y + (vec.y - y) * t);
		}

		inline float invLerp(Vector2 const& mid, Vector2 const& end) const
		{
			float dEnd = distanceTo(end);
			if (dEnd == 0.0f)
			{
				return distanceTo(mid);
			}
			else
			{
				return distanceTo(mid) / dEnd;
			}
		}

		inline Vector2 perpendicular() const
		{
			return Vector2(-y, x);
		}

		inline float anticlockwiseAngleTo(Vector2 const& vec) const
		{
			float angle = atan2(vec.y, vec.x) - atan2(y, x);

			if (angle < 0.0f)
			{
				angle += 3.14159f * 2;
			}

			return angle * 180.0f / 3.14159f;
		}

		inline float clockwiseAngleTo(Vector2 const& vec) const
		{
			float angle = atan2(y, x) - atan2(vec.y, vec.x);

			if (angle < 0.0f)
			{
				angle += 3.14159f * 2;
			}

			return angle * 180.0f / 3.14159f;
		}

		inline float clockwiseAngle() const
		{
			float angle = 3.14159f * 0.5f - atan2(y, x); // atan2(1, 0) = PI/2

			if (angle < 0.0f)
			{
				angle += 3.14159f * 2;
			}

			return angle * 3.14159f / 180.0f;
		}

		inline float minimumAngleTo(Vector2 const& vec) const
		{
			float angle = anticlockwiseAngleTo(vec);

			if (angle >= 180.0f)
			{
				angle = 360.0f - angle;
			}

			return angle;
		}

		inline void rotate(float angle)
		{
			float angleRadians = angle * 3.14159f / 180.0f;

			float nx = x * cos(angleRadians) - y * sin(angleRadians);
			float ny = x * sin(angleRadians) + y * cos(angleRadians);

			x = nx;
			y = ny;
		}

		inline void rotateAnticlockwise(float angle)
		{
			rotate(angle);
		}

		inline void rotateClockwise(float angle)
		{
			rotate(-angle);
		}

		inline Vector2 rotatedCopy(float angle) const
		{
			float angleRadians = angle * 3.14159f / 180.0f;
			float sinAngle = sin(angleRadians);
			float cosAngle = cos(angleRadians);

			float nx = x * cosAngle - y * sinAngle;
			float ny = x * sinAngle + y * cosAngle;

			return Vector2(nx, ny);
		}

		inline Vector2 rotatedAnticlockwiseCopy(float angle)
		{
			return rotatedCopy(angle);
		}

		inline Vector2 rotatedClockwiseCopy(float angle)
		{
			return rotatedCopy(-angle);
		}

		inline void rotateAround(Vector2 const& origin, float angle)
		{
			x -= origin.x;
			y -= origin.y;

			float angleRadians = angle * 3.14159f / 180.0f;

			float nx = x * cos(angleRadians) - y * sin(angleRadians);
			float ny = x * sin(angleRadians) + y * cos(angleRadians);

			x = nx + origin.x;
			y = ny + origin.y;
		}

		inline void rotateAnticlockwiseAround(Vector2 const& origin, float angle)
		{
			rotateAround(origin, angle);
		}

		inline void rotateClockwiseAround(Vector2 const& origin, float angle)
		{
			rotateAround(origin, -angle);
		}

		// Constants
		static const Vector2 ZERO;
		static const Vector2 UNIT_X;
		static const Vector2 UNIT_Y;
		static const Vector2 NEGATIVE_UNIT_X;
		static const Vector2 NEGATIVE_UNIT_Y;
	};

	struct Vector2Compare
	{
		bool operator()(Vector2 const& a, Vector2 const& b) const
		{
			if (a.x < b.x)
			{
				return false;
			}
			else if (a.x > b.x)
			{
				return true;
			}
			else
			{
				return a.y < b.y;
			}
		}
	};

} // core


// Global operator
core::Vector2 operator*(float value, core::Vector2 const& vec);
