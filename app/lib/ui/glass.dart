import 'dart:ui';

import 'package:flutter/material.dart';

class GlassSurface extends StatelessWidget {
  const GlassSurface({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(24),
    this.radius = 30,
    this.blur = 12,
  });
  final Widget child;
  final EdgeInsetsGeometry padding;
  final double radius;
  final double blur;

  @override
  Widget build(BuildContext context) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    return DecoratedBox(
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(radius),
        boxShadow: [
          BoxShadow(
            color: dark
                ? Colors.black.withValues(alpha: .18)
                : const Color(0xFF487878).withValues(alpha: .10),
            blurRadius: 30,
            offset: const Offset(0, 12),
          ),
        ],
      ),
      child: ClipRRect(
        borderRadius: BorderRadius.circular(radius),
        child: BackdropFilter(
          filter: ImageFilter.blur(sigmaX: blur, sigmaY: blur),
          child: DecoratedBox(
            decoration: BoxDecoration(
              color: dark
                  ? const Color(0xFF262626).withValues(alpha: .94)
                  : Colors.white.withValues(alpha: .65),
              borderRadius: BorderRadius.circular(radius),
              border: Border.all(
                color: dark
                    ? const Color(0xFF404040)
                    : Colors.white.withValues(alpha: .95),
                width: 1.2,
              ),
            ),
            child: Padding(padding: padding, child: child),
          ),
        ),
      ),
    );
  }
}

class BrandMark extends StatelessWidget {
  const BrandMark({super.key, this.size = 54, this.wordmark = true});
  final double size;
  final bool wordmark;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Semantics(
          label: 'HyGrow logo',
          image: true,
          child: Container(
            width: size,
            height: size,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(size * .25),
              boxShadow: [
                BoxShadow(
                  color: Colors.black.withValues(alpha: .06),
                  blurRadius: 12,
                  offset: const Offset(0, 5),
                ),
              ],
            ),
            child: ClipRRect(
              borderRadius: BorderRadius.circular(size * .25),
              child: Transform.scale(
                scale: 1.12,
                child: Image.asset(
                  'assets/branding/app-icon.png',
                  width: size,
                  height: size,
                  cacheWidth: 256,
                  fit: BoxFit.cover,
                ),
              ),
            ),
          ),
        ),
        if (wordmark) ...[
          const SizedBox(width: 12),
          Text(
            'HyGrow',
            style: TextStyle(
              fontSize: size * .47,
              fontWeight: FontWeight.w800,
              letterSpacing: -.8,
              color: Theme.of(context).colorScheme.onSurface,
            ),
          ),
        ],
      ],
    );
  }
}

class BotanicalCanvas extends StatelessWidget {
  const BotanicalCanvas({super.key, required this.child});
  final Widget child;
  @override
  Widget build(BuildContext context) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    return DecoratedBox(
      decoration: BoxDecoration(color: Theme.of(context).colorScheme.surface),
      child: Stack(
        children: [
          if (!dark)
            Positioned.fill(
              child: IgnorePointer(child: CustomPaint(painter: _MintWaves())),
            ),
          Positioned.fill(child: child),
        ],
      ),
    );
  }
}

class _MintWaves extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..shader = const LinearGradient(
        colors: [Color(0x337AD9B3), Color(0x077AD9B3)],
        begin: Alignment.bottomLeft,
        end: Alignment.topRight,
      ).createShader(Offset.zero & size);
    final path = Path()
      ..moveTo(0, size.height * .84)
      ..cubicTo(
        size.width * .36,
        size.height * .72,
        size.width * .55,
        size.height * 1.03,
        size.width,
        size.height * .85,
      )
      ..lineTo(size.width, size.height)
      ..lineTo(0, size.height)
      ..close();
    canvas.drawPath(path, paint);
    canvas.drawCircle(
      Offset(size.width * 1.05, size.height * .03),
      size.width * .44,
      Paint()..color = const Color(0x147AD9B3),
    );
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
