import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:go_router/go_router.dart';

import '../auth/auth_controller.dart';
import '../ui/glass.dart';

enum AuthStep { login, signup, verify, forgot, reset }

class AuthScreen extends StatefulWidget {
  const AuthScreen({
    super.key,
    required this.auth,
    required this.step,
    this.challengeId,
    this.email,
  });
  final AuthController auth;
  final AuthStep step;
  final String? challengeId;
  final String? email;
  @override
  State<AuthScreen> createState() => _AuthScreenState();
}

class _AuthScreenState extends State<AuthScreen> {
  final _form = GlobalKey<FormState>();
  final _name = TextEditingController();
  final _email = TextEditingController();
  final _password = TextEditingController();
  final _confirm = TextEditingController();
  final _code = TextEditingController();
  bool _busy = false;
  bool _hidden = true;
  String? _error;

  @override
  void initState() {
    super.initState();
    _email.text = widget.email ?? '';
  }

  @override
  void dispose() {
    for (final item in [_name, _email, _password, _confirm, _code]) {
      item.dispose();
    }
    super.dispose();
  }

  String get title => switch (widget.step) {
    AuthStep.login => 'Welcome back',
    AuthStep.signup => 'Create farmer account',
    AuthStep.verify => 'Verify your email',
    AuthStep.forgot => 'Forgot password?',
    AuthStep.reset => 'Reset your password',
  };
  String get action => switch (widget.step) {
    AuthStep.login => 'Log in',
    AuthStep.signup => 'Sign up',
    AuthStep.verify => 'Verify email',
    AuthStep.forgot => 'Send code',
    AuthStep.reset => 'Reset password',
  };

  Future<void> _submit() async {
    if (_busy || !_form.currentState!.validate()) return;
    FocusScope.of(context).unfocus();
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      switch (widget.step) {
        case AuthStep.login:
          await widget.auth.login(_email.text, _password.text);
          if (mounted) context.go('/farmer/devices');
        case AuthStep.signup:
          final id = await widget.auth.signup(
            _name.text,
            _email.text,
            _password.text,
          );
          if (mounted) {
            context.push(
              '/verify',
              extra: {'challengeId': id, 'email': _email.text.trim()},
            );
          }
        case AuthStep.verify:
          if (widget.challengeId == null) {
            throw const ApiFailure(
              'Start sign up again to request a new code.',
            );
          }
          await widget.auth.verify(widget.challengeId!, _code.text);
          if (mounted) context.go('/farmer/devices');
        case AuthStep.forgot:
          final id = await widget.auth.forgot(_email.text);
          if (mounted) {
            context.push(
              '/reset',
              extra: {'challengeId': id, 'email': _email.text.trim()},
            );
          }
        case AuthStep.reset:
          if (widget.challengeId == null) {
            throw const ApiFailure('Request a new reset code.');
          }
          await widget.auth.reset(
            widget.challengeId!,
            _code.text,
            _password.text,
          );
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(
                content: Text('Password updated. You can now log in.'),
              ),
            );
            context.go('/login');
          }
      }
      TextInput.finishAutofillContext();
    } on ApiFailure catch (error) {
      if (mounted) setState(() => _error = error.toString());
    } catch (_) {
      if (mounted) {
        setState(
          () => _error = 'We could not complete this request. Please retry.',
        );
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final signup = widget.step == AuthStep.signup;
    final password = [
      AuthStep.login,
      AuthStep.signup,
      AuthStep.reset,
    ].contains(widget.step);
    final code = [AuthStep.verify, AuthStep.reset].contains(widget.step);
    return Scaffold(
      body: BotanicalCanvas(
        child: SafeArea(
          child: SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(24, 20, 24, 32),
            child: Center(
              child: ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 620),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      children: [
                        GlassSurface(
                          radius: 19,
                          padding: EdgeInsets.zero,
                          child: IconButton(
                            tooltip: 'Back',
                            onPressed: _busy
                                ? null
                                : () => context.canPop()
                                      ? context.pop()
                                      : context.go('/welcome'),
                            icon: const Icon(
                              Icons.arrow_back_ios_new_rounded,
                              size: 20,
                            ),
                          ),
                        ),
                        const Spacer(),
                        const BrandMark(size: 52),
                        const Spacer(),
                        const SizedBox(width: 48),
                      ],
                    ),
                    const SizedBox(height: 32),
                    SizedBox(
                      height: 170,
                      child: Stack(
                        children: [
                          Positioned(
                            right: -24,
                            top: 0,
                            bottom: 0,
                            width: 240,
                            child: ClipRRect(
                              borderRadius: BorderRadius.circular(28),
                              child: Image.asset(
                                'assets/artwork/onboarding/greenhouse.png',
                                fit: BoxFit.cover,
                                alignment: Alignment.centerRight,
                                cacheWidth: 600,
                              ),
                            ),
                          ),
                          Positioned.fill(
                            child: DecoratedBox(
                              decoration: BoxDecoration(
                                gradient: LinearGradient(
                                  colors: [
                                    Theme.of(context).colorScheme.surface,
                                    Theme.of(context).colorScheme.surface
                                        .withValues(alpha: .95),
                                    Theme.of(context).colorScheme.surface
                                        .withValues(alpha: 0),
                                  ],
                                  stops: const [0, .35, 1],
                                ),
                              ),
                            ),
                          ),
                          Align(
                            alignment: Alignment.centerLeft,
                            child: SizedBox(
                              width: 245,
                              child: Column(
                                mainAxisSize: MainAxisSize.min,
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Container(
                                    padding: const EdgeInsets.symmetric(
                                      horizontal: 12,
                                      vertical: 7,
                                    ),
                                    decoration: BoxDecoration(
                                      color: Theme.of(context)
                                          .colorScheme
                                          .surfaceContainerHigh,
                                      borderRadius: BorderRadius.circular(24),
                                    ),
                                    child: Row(
                                      mainAxisSize: MainAxisSize.min,
                                      children: [
                                        Icon(
                                          Icons.agriculture_outlined,
                                          size: 18,
                                          color: Theme.of(context)
                                              .colorScheme
                                              .primary,
                                        ),
                                        const SizedBox(width: 6),
                                        const Text('For farmers'),
                                      ],
                                    ),
                                  ),
                                  const SizedBox(height: 14),
                                  Text(
                                    title,
                                    style: Theme.of(context)
                                        .textTheme
                                        .headlineLarge,
                                  ),
                                ],
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 24),
                    GlassSurface(
                      padding: const EdgeInsets.all(20),
                      child: AutofillGroup(
                        child: Form(
                          key: _form,
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: [
                              if (signup) ...[
                                TextFormField(
                                  controller: _name,
                                  enabled: !_busy,
                                  textCapitalization: TextCapitalization.words,
                                  autofillHints: const [AutofillHints.name],
                                  decoration: const InputDecoration(
                                    labelText: 'Full name',
                                    prefixIcon: Icon(
                                      Icons.person_outline_rounded,
                                    ),
                                  ),
                                  validator: (value) =>
                                      value == null || value.trim().isEmpty
                                      ? 'Enter your name.'
                                      : null,
                                ),
                                const SizedBox(height: 18),
                              ],
                              if (!code) ...[
                                TextFormField(
                                  controller: _email,
                                  enabled: !_busy,
                                  keyboardType: TextInputType.emailAddress,
                                  autocorrect: false,
                                  autofillHints: const [AutofillHints.email],
                                  decoration: const InputDecoration(
                                    labelText: 'Email address',
                                    prefixIcon: Icon(
                                      Icons.mail_outline_rounded,
                                    ),
                                  ),
                                  validator: (value) =>
                                      value == null ||
                                          !RegExp(r'^[^\s@]+@[^\s@]+\.[^\s@]+$')
                                              .hasMatch(value.trim())
                                      ? 'Enter your email address.'
                                      : null,
                                ),
                                const SizedBox(height: 18),
                              ],
                              if (code) ...[
                                Text(
                                  widget.email == null
                                      ? 'Enter the six-digit code from your email.'
                                      : 'Enter the six-digit code sent to ${widget.email}.',
                                  textAlign: TextAlign.center,
                                ),
                                const SizedBox(height: 20),
                                TextFormField(
                                  controller: _code,
                                  enabled: !_busy,
                                  keyboardType: TextInputType.number,
                                  autofillHints: const [
                                    AutofillHints.oneTimeCode,
                                  ],
                                  inputFormatters: [
                                    FilteringTextInputFormatter.digitsOnly,
                                    LengthLimitingTextInputFormatter(6),
                                  ],
                                  textAlign: TextAlign.center,
                                  style: const TextStyle(
                                    fontSize: 25,
                                    fontWeight: FontWeight.w700,
                                    letterSpacing: 10,
                                  ),
                                  decoration: const InputDecoration(
                                    labelText: 'Verification code',
                                  ),
                                  validator: (value) => value?.length != 6
                                      ? 'Enter all six digits.'
                                      : null,
                                ),
                                const SizedBox(height: 18),
                              ],
                              if (password) ...[
                                TextFormField(
                                  controller: _password,
                                  enabled: !_busy,
                                  obscureText: _hidden,
                                  autocorrect: false,
                                  enableSuggestions: false,
                                  autofillHints: [
                                    signup || widget.step == AuthStep.reset
                                        ? AutofillHints.newPassword
                                        : AutofillHints.password,
                                  ],
                                  decoration: InputDecoration(
                                    labelText: 'Password',
                                    prefixIcon: const Icon(
                                      Icons.lock_outline_rounded,
                                    ),
                                    suffixIcon: IconButton(
                                      tooltip: _hidden
                                          ? 'Show password'
                                          : 'Hide password',
                                      onPressed: () =>
                                          setState(() => _hidden = !_hidden),
                                      icon: Icon(
                                        _hidden
                                            ? Icons.visibility_off_outlined
                                            : Icons.visibility_outlined,
                                      ),
                                    ),
                                  ),
                                  validator: (value) =>
                                      value == null ||
                                          value.length <
                                              (widget.step == AuthStep.login
                                                  ? 1
                                                  : 12)
                                      ? (widget.step == AuthStep.login
                                            ? 'Enter your password.'
                                            : 'Use at least 12 characters.')
                                      : null,
                                ),
                                const SizedBox(height: 18),
                              ],
                              if (signup || widget.step == AuthStep.reset) ...[
                                TextFormField(
                                  controller: _confirm,
                                  enabled: !_busy,
                                  obscureText: _hidden,
                                  autocorrect: false,
                                  enableSuggestions: false,
                                  decoration: const InputDecoration(
                                    labelText: 'Confirm password',
                                    prefixIcon: Icon(
                                      Icons.lock_outline_rounded,
                                    ),
                                  ),
                                  validator: (value) => value != _password.text
                                      ? 'Passwords do not match.'
                                      : null,
                                ),
                                const SizedBox(height: 18),
                              ],
                              if (widget.step == AuthStep.login)
                                Align(
                                  alignment: Alignment.centerRight,
                                  child: TextButton(
                                    onPressed: _busy
                                        ? null
                                        : () => context.push('/forgot'),
                                    child: const Text('Forgot password?'),
                                  ),
                                ),
                              if (_error != null) ...[
                                Semantics(
                                  liveRegion: true,
                                  child: Container(
                                    padding: const EdgeInsets.all(14),
                                    decoration: BoxDecoration(
                                      color: Theme.of(context)
                                          .colorScheme
                                          .errorContainer,
                                      borderRadius: BorderRadius.circular(16),
                                    ),
                                    child: Text(
                                      _error!,
                                      style: TextStyle(
                                        color: Theme.of(context)
                                            .colorScheme
                                            .onErrorContainer,
                                      ),
                                    ),
                                  ),
                                ),
                                const SizedBox(height: 18),
                              ],
                              FilledButton(
                                onPressed: _busy ? null : _submit,
                                child: AnimatedSwitcher(
                                  duration:
                                      MediaQuery.disableAnimationsOf(context)
                                      ? Duration.zero
                                      : const Duration(milliseconds: 160),
                                  child: _busy
                                      ? const SizedBox(
                                          width: 22,
                                          height: 22,
                                          child: CircularProgressIndicator(
                                            strokeWidth: 2,
                                          ),
                                        )
                                      : Row(
                                          mainAxisAlignment:
                                              MainAxisAlignment.center,
                                          children: [
                                            Text(action),
                                            const SizedBox(width: 12),
                                            const Icon(
                                              Icons.arrow_forward_rounded,
                                              size: 21,
                                            ),
                                          ],
                                        ),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                    const SizedBox(height: 22),
                    if (widget.step == AuthStep.login || signup)
                      Wrap(
                        alignment: WrapAlignment.center,
                        crossAxisAlignment: WrapCrossAlignment.center,
                        children: [
                          Text(
                            signup
                                ? 'Already have an account?'
                                : 'New to HyGrow?',
                          ),
                          TextButton(
                            onPressed: _busy
                                ? null
                                : () =>
                                      context.go(signup ? '/login' : '/signup'),
                            child: Text(signup ? 'Log in' : 'Sign up'),
                          ),
                        ],
                      ),
                    if (widget.step == AuthStep.forgot)
                      TextButton(
                        onPressed: () => context.go('/login'),
                        child: const Text('Back to login'),
                      ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
