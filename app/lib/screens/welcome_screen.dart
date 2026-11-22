import 'package:flutter/material.dart';

import '../ui/glass.dart';

class WelcomeScreen extends StatelessWidget {
  const WelcomeScreen({super.key, required this.onFarmer});
  final VoidCallback onFarmer;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: BotanicalCanvas(
        child: SafeArea(
          child: LayoutBuilder(
            builder: (context, constraints) {
              final wide = constraints.maxWidth >= 800;
              final largeText = MediaQuery.textScalerOf(context).scale(14) > 18;
              return SingleChildScrollView(
                padding: EdgeInsets.symmetric(
                  horizontal: wide ? 48 : 24,
                  vertical: 28,
                ),
                child: Center(
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: 1100),
                    child: Column(
                      children: [
                        const BrandMark(size: 76, wordmark: false),
                        const SizedBox(height: 16),
                        Text(
                          'HyGrow',
                          style: Theme.of(context).textTheme.headlineLarge
                              ?.copyWith(fontSize: 34),
                        ),
                        const SizedBox(height: 8),
                        const Text(
                          'Smart farming. Fresh possibilities.',
                          textAlign: TextAlign.center,
                        ),
                        const SizedBox(height: 28),
                        if (wide)
                          Row(
                            crossAxisAlignment: CrossAxisAlignment.center,
                            children: [
                              const Expanded(
                                flex: 6,
                                child: _GreenhouseImage(height: 400),
                              ),
                              const SizedBox(width: 32),
                              Expanded(
                                flex: 5,
                                child: _Choices(
                                  onFarmer: onFarmer,
                                  vertical: largeText,
                                ),
                              ),
                            ],
                          )
                        else ...[
                          _GreenhouseImage(
                            height: constraints.maxHeight > 760 ? 230 : 190,
                          ),
                          const SizedBox(height: 26),
                          _Choices(onFarmer: onFarmer, vertical: largeText),
                        ],
                        const SizedBox(height: 20),
                      ],
                    ),
                  ),
                ),
              );
            },
          ),
        ),
      ),
    );
  }
}

class _GreenhouseImage extends StatelessWidget {
  const _GreenhouseImage({required this.height});
  final double height;
  @override
  Widget build(BuildContext context) => Semantics(
    image: true,
    label: 'Lettuce growing in a hydroponic greenhouse',
    child: ClipRRect(
      borderRadius: BorderRadius.circular(34),
      child: SizedBox(
        width: double.infinity,
        height: height,
        child: Image.asset(
          'assets/artwork/onboarding/greenhouse.png',
          fit: BoxFit.cover,
          alignment: const Alignment(.45, .2),
          cacheWidth: 1200,
        ),
      ),
    ),
  );
}

class _Choices extends StatelessWidget {
  const _Choices({required this.onFarmer, required this.vertical});
  final VoidCallback onFarmer;
  final bool vertical;
  @override
  Widget build(BuildContext context) {
    final farmer = _RoleCard(
      title: 'Farmer',
      description: 'Monitor, manage and grow.',
      icon: Icons.agriculture_outlined,
      image: 'assets/artwork/onboarding/greenhouse.png',
      action: 'Continue as Farmer',
      onPressed: onFarmer,
    );
    const buyer = _RoleCard(
      title: 'Buyer',
      description: 'Discover fresh farm produce.',
      icon: Icons.shopping_basket_outlined,
      image: 'assets/artwork/onboarding/fresh-produce.png',
      action: 'Coming soon',
    );
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(
          'Choose your role',
          textAlign: TextAlign.center,
          style: Theme.of(context).textTheme.headlineMedium,
        ),
        const SizedBox(height: 20),
        if (vertical) ...[
          farmer,
          const SizedBox(height: 16),
          buyer,
        ] else
          IntrinsicHeight(
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Expanded(child: farmer),
                const SizedBox(width: 14),
                const Expanded(child: buyer),
              ],
            ),
          ),
      ],
    );
  }
}

class _RoleCard extends StatelessWidget {
  const _RoleCard({
    required this.title,
    required this.description,
    required this.icon,
    required this.image,
    required this.action,
    this.onPressed,
  });
  final String title;
  final String description;
  final IconData icon;
  final String image;
  final String action;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    return GlassSurface(
      radius: 30,
      padding: const EdgeInsets.fromLTRB(12, 12, 12, 16),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          ClipRRect(
            borderRadius: BorderRadius.circular(21),
            child: Image.asset(
              image,
              width: double.infinity,
              height: 94,
              fit: BoxFit.cover,
              cacheWidth: 600,
              excludeFromSemantics: true,
            ),
          ),
          const SizedBox(height: 12),
          Icon(
            icon,
            size: 30,
            color: dark ? Colors.white : const Color(0xFF14734B),
          ),
          const SizedBox(height: 8),
          Text(title, style: Theme.of(context).textTheme.titleLarge),
          const SizedBox(height: 8),
          ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 42),
            child: Text(
              description,
              textAlign: TextAlign.center,
              style: Theme.of(context).textTheme.bodyMedium,
            ),
          ),
          const SizedBox(height: 16),
          FilledButton(
            style: FilledButton.styleFrom(
              minimumSize: const Size.fromHeight(50),
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 14),
              textStyle: const TextStyle(
                fontSize: 12.5,
                fontWeight: FontWeight.w700,
              ),
            ),
            onPressed: onPressed,
            child: Text(action, textAlign: TextAlign.center),
          ),
        ],
      ),
    );
  }
}
