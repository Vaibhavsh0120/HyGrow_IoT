import 'package:flutter/material.dart';

class HyGrowTheme {
  static const mint = Color(0xFF087B4D);
  static ThemeData light() => _make(Brightness.light);
  static ThemeData dark() => _make(Brightness.dark);

  static ThemeData _make(Brightness brightness) {
    final dark = brightness == Brightness.dark;
    final scheme = ColorScheme.fromSeed(seedColor: mint, brightness: brightness)
        .copyWith(
          primary: dark ? const Color(0xFFF5F5F5) : mint,
          onPrimary: dark ? const Color(0xFF171717) : Colors.white,
          surface: dark ? const Color(0xFF171717) : const Color(0xFFF4F8FA),
          onSurface: dark ? const Color(0xFFF5F5F5) : const Color(0xFF14212B),
          onSurfaceVariant: dark
              ? const Color(0xFFA3A3A3)
              : const Color(0xFF5A6B7E),
          surfaceContainerLowest: dark ? const Color(0xFF171717) : Colors.white,
          surfaceContainerLow: dark
              ? const Color(0xFF202020)
              : const Color(0xFFF7FAFB),
          surfaceContainer: dark
              ? const Color(0xFF262626)
              : const Color(0xFFF1F7F6),
          surfaceContainerHigh: dark ? const Color(0xFF303030) : Colors.white,
          surfaceContainerHighest: dark
              ? const Color(0xFF353535)
              : const Color(0xFFE7F0EE),
          outline: dark ? const Color(0xFF404040) : const Color(0xFFCFDFDC),
          outlineVariant: dark
              ? const Color(0xFF404040)
              : const Color(0xFFDDE7E5),
          shadow: Colors.black,
          surfaceTint: Colors.transparent,
        );
    final base = ThemeData(
      useMaterial3: true,
      colorScheme: scheme,
      scaffoldBackgroundColor: scheme.surface,
    );
    return base.copyWith(
      textTheme: base.textTheme.copyWith(
        headlineLarge: TextStyle(
          fontSize: 32,
          fontWeight: FontWeight.w800,
          letterSpacing: -1.2,
          height: 1.14,
          color: scheme.onSurface,
        ),
        headlineMedium: TextStyle(
          fontSize: 26,
          fontWeight: FontWeight.w800,
          letterSpacing: -.8,
          height: 1.2,
          color: scheme.onSurface,
        ),
        titleLarge: TextStyle(
          fontSize: 21,
          fontWeight: FontWeight.w700,
          letterSpacing: -.4,
          color: scheme.onSurface,
        ),
        bodyLarge: TextStyle(
          fontSize: 16,
          height: 1.5,
          color: scheme.onSurfaceVariant,
        ),
        bodyMedium: TextStyle(
          fontSize: 14,
          height: 1.45,
          color: scheme.onSurfaceVariant,
        ),
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true,
        fillColor: dark
            ? const Color(0xFF303030)
            : Colors.white.withValues(alpha: .7),
        contentPadding: const EdgeInsets.symmetric(
          horizontal: 20,
          vertical: 20,
        ),
        border: OutlineInputBorder(
          borderRadius: BorderRadius.circular(22),
          borderSide: BorderSide(color: scheme.outlineVariant),
        ),
        enabledBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(22),
          borderSide: BorderSide(color: dark ? scheme.outline : Colors.white),
        ),
        focusedBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(22),
          borderSide: BorderSide(color: scheme.primary, width: 1.6),
        ),
        prefixIconColor: scheme.primary,
      ),
      filledButtonTheme: FilledButtonThemeData(
        style: FilledButton.styleFrom(
          minimumSize: const Size.fromHeight(56),
          textStyle: const TextStyle(fontSize: 17, fontWeight: FontWeight.w700),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(24),
          ),
        ),
      ),
      iconButtonTheme: IconButtonThemeData(
        style: IconButton.styleFrom(
          minimumSize: const Size(48, 48),
          foregroundColor: scheme.onSurface,
        ),
      ),
      snackBarTheme: SnackBarThemeData(
        behavior: SnackBarBehavior.floating,
        backgroundColor: dark
            ? const Color(0xFF404040)
            : const Color(0xFF142D25),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      ),
    );
  }
}
