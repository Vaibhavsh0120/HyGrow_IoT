import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:go_router/go_router.dart';

import 'auth/auth_controller.dart';
import 'screens/welcome_screen.dart';
import 'screens/auth_screen.dart';
import 'screens/devices_screen.dart';
import 'ui/glass.dart';
import 'ui/theme.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(
    HyGrowApp(
      auth: AuthController(
        api: HttpHyGrowApi(
          Uri.parse(
            const String.fromEnvironment(
              'API_BASE_URL',
              defaultValue: 'https://hygrow-india.duckdns.org',
            ),
          ),
        ),
        store: SecureSessionStore(),
      ),
    ),
  );
}

class HyGrowApp extends StatefulWidget {
  const HyGrowApp({super.key, required this.auth});
  final AuthController auth;
  @override
  State<HyGrowApp> createState() => _HyGrowAppState();
}

class _HyGrowAppState extends State<HyGrowApp> {
  late final GoRouter _router;
  @override
  void initState() {
    super.initState();
    _router = GoRouter(
      initialLocation: '/welcome',
      refreshListenable: widget.auth,
      redirect: (context, state) {
        final location = state.matchedLocation;
        if (widget.auth.restoring) {
          return location == '/splash' ? null : '/splash';
        }
        if (location == '/splash') {
          return widget.auth.signedIn ? '/farmer/devices' : '/welcome';
        }
        if (!widget.auth.signedIn &&
            (location.startsWith('/farmer') || location == '/settings')) {
          return '/welcome';
        }
        if (widget.auth.signedIn &&
            ['/welcome', '/login', '/signup', '/verify'].contains(location)) {
          return '/farmer/devices';
        }
        return null;
      },
      routes: [
        GoRoute(path: '/splash', builder: (_, _) => const _SplashScreen()),
        GoRoute(
          path: '/welcome',
          builder: (context, _) => ListenableBuilder(
            listenable: widget.auth,
            builder: (_, _) => WelcomeScreen(
              onFarmer: () => context.push('/login'),
              notice: widget.auth.signOutWarning,
              onRetry: () => widget.auth.signOut(),
            ),
          ),
        ),
        GoRoute(
          path: '/login',
          builder: (_, _) =>
              AuthScreen(auth: widget.auth, step: AuthStep.login),
        ),
        GoRoute(
          path: '/signup',
          builder: (_, _) =>
              AuthScreen(auth: widget.auth, step: AuthStep.signup),
        ),
        GoRoute(
          path: '/forgot',
          builder: (_, _) =>
              AuthScreen(auth: widget.auth, step: AuthStep.forgot),
        ),
        GoRoute(
          path: '/verify',
          builder: (_, state) {
            final data = state.extra as Map<String, String>?;
            return AuthScreen(
              auth: widget.auth,
              step: AuthStep.verify,
              challengeId: data?['challengeId'],
              email: data?['email'],
            );
          },
        ),
        GoRoute(
          path: '/reset',
          builder: (_, state) {
            final data = state.extra as Map<String, String>?;
            return AuthScreen(
              auth: widget.auth,
              step: AuthStep.reset,
              challengeId: data?['challengeId'],
              email: data?['email'],
            );
          },
        ),
        GoRoute(
          path: '/farmer/devices',
          builder: (_, _) => DevicesScreen(auth: widget.auth),
        ),
        GoRoute(
          path: '/settings',
          builder: (_, _) => CommonSettingsScreen(auth: widget.auth),
        ),
      ],
    );
    unawaited(widget.auth.bootstrap());
  }

  @override
  void dispose() {
    _router.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: widget.auth,
    builder: (_, _) => MaterialApp.router(
      title: 'HyGrow',
      debugShowCheckedModeBanner: false,
      theme: HyGrowTheme.light(),
      darkTheme: HyGrowTheme.dark(),
      themeMode: widget.auth.themeMode,
      builder: (context, child) {
        final dark = Theme.of(context).brightness == Brightness.dark;
        return AnnotatedRegion<SystemUiOverlayStyle>(
          value: SystemUiOverlayStyle(
            statusBarColor: Colors.transparent,
            statusBarIconBrightness: dark ? Brightness.light : Brightness.dark,
            statusBarBrightness: dark ? Brightness.dark : Brightness.light,
            systemNavigationBarColor: Theme.of(context).colorScheme.surface,
            systemNavigationBarIconBrightness: dark
                ? Brightness.light
                : Brightness.dark,
          ),
          child: child!,
        );
      },
      routerConfig: _router,
    ),
  );
}

class _SplashScreen extends StatelessWidget {
  const _SplashScreen();
  @override
  Widget build(BuildContext context) => Scaffold(
    body: Center(
      child: Semantics(
        label: 'HyGrow. Restoring your session.',
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const BrandMark(size: 112, wordmark: false),
            const SizedBox(height: 24),
            Text('HyGrow', style: Theme.of(context).textTheme.headlineLarge),
          ],
        ),
      ),
    ),
  );
}
