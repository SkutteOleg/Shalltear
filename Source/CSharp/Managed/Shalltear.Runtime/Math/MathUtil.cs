namespace Shalltear;

// must match MathUtil.h (same comparisons -> NaN/-0 agree)
internal static class MathUtil {
	internal const float PI = 3.14159265358979323846f;
	internal const float DEG_TO_RAD = PI / 180.0f;
	internal const float RAD_TO_DEG = 180.0f / PI;
	internal const float EPSILON = 1e-5f;

	internal static float Abs(float value) {
		return value < 0.0f ? -value : value;
	}

	internal static float Min(float a, float b) {
		return a < b ? a : b;
	}

	internal static float Max(float a, float b) {
		return a > b ? a : b;
	}

	internal static float Clamp(float value, float min, float max) {
		return value < min ? min : (value > max ? max : value);
	}

	internal static float Clamp01(float value) {
		return Clamp(value, 0.0f, 1.0f);
	}

	internal static bool Approximately(float a, float b, float epsilon) {
		return Abs(a - b) <= epsilon;
	}
}
