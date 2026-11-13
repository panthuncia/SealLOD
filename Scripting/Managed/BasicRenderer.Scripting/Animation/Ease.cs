using System;

namespace BasicRenderer.Scripting;

/// <summary>Maps linear progress t in [0, 1] to eased progress (0 at 0, 1 at 1).</summary>
public delegate double Easing(double t);

/// <summary>Standard easing curves (https://easings.net names).</summary>
public static class Ease
{
    public static readonly Easing Linear = t => t;

    public static readonly Easing InQuad = t => t * t;
    public static readonly Easing OutQuad = t => 1 - (1 - t) * (1 - t);
    public static readonly Easing InOutQuad = t => t < 0.5 ? 2 * t * t : 1 - Math.Pow(-2 * t + 2, 2) / 2;

    public static readonly Easing InCubic = t => t * t * t;
    public static readonly Easing OutCubic = t => 1 - Math.Pow(1 - t, 3);
    public static readonly Easing InOutCubic = t => t < 0.5 ? 4 * t * t * t : 1 - Math.Pow(-2 * t + 2, 3) / 2;

    public static readonly Easing InSine = t => 1 - Math.Cos(t * Math.PI / 2);
    public static readonly Easing OutSine = t => Math.Sin(t * Math.PI / 2);
    public static readonly Easing InOutSine = t => -(Math.Cos(Math.PI * t) - 1) / 2;

    /// <summary>Smoothstep: zero velocity at both ends.</summary>
    public static readonly Easing Smooth = t => t * t * (3 - 2 * t);

    /// <summary>Overshoots slightly, then settles.</summary>
    public static readonly Easing OutBack = t =>
    {
        const double c1 = 1.70158, c3 = c1 + 1;
        return 1 + c3 * Math.Pow(t - 1, 3) + c1 * Math.Pow(t - 1, 2);
    };

    internal static double Apply(Easing? easing, double t) => (easing ?? Linear)(Math.Clamp(t, 0.0, 1.0));
}
