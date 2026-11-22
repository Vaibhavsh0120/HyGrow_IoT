import 'package:flutter/material.dart';

import 'glass.dart';

@immutable
class GlassDestination {
  const GlassDestination({required this.icon, required this.label});
  final IconData icon;
  final String label;
}

class HyGrowNavBar extends StatelessWidget {
  const HyGrowNavBar({
    super.key,
    required this.destinations,
    required this.selectedIndex,
    required this.onSelected,
  }) : assert(destinations.length >= 2),
       assert(selectedIndex >= 0 && selectedIndex < destinations.length);
  final List<GlassDestination> destinations;
  final int selectedIndex;
  final ValueChanged<int> onSelected;

  @override
  Widget build(BuildContext context) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    final reducedMotion = MediaQuery.disableAnimationsOf(context);
    return SafeArea(
      top: false,
      minimum: const EdgeInsets.fromLTRB(16, 8, 16, 12),
      child: Center(
        heightFactor: 1,
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 760),
          child: GlassSurface(
            radius: 30,
            padding: const EdgeInsets.all(7),
            blur: 18,
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: List.generate(destinations.length, (index) {
                final destination = destinations[index];
                final selected = index == selectedIndex;
                final foreground = selected
                    ? (dark ? const Color(0xFFF5F5F5) : const Color(0xFF087B4D))
                    : Theme.of(context).colorScheme.onSurfaceVariant;
                return Expanded(
                  child: Semantics(
                    selected: selected,
                    button: true,
                    label: destination.label,
                    child: Tooltip(
                      message: destination.label,
                      child: Material(
                        color: Colors.transparent,
                        child: InkWell(
                          borderRadius: BorderRadius.circular(23),
                          onTap: () => onSelected(index),
                          child: AnimatedContainer(
                            duration: reducedMotion
                                ? Duration.zero
                                : const Duration(milliseconds: 180),
                            curve: Curves.easeOutCubic,
                            padding: const EdgeInsets.symmetric(
                              horizontal: 3,
                              vertical: 12,
                            ),
                            constraints: const BoxConstraints(minHeight: 76),
                            decoration: BoxDecoration(
                              color: selected
                                  ? (dark
                                        ? const Color(0xFF353535)
                                        : Colors.white.withValues(alpha: .8))
                                  : Colors.transparent,
                              borderRadius: BorderRadius.circular(23),
                              border: Border.all(
                                color: selected
                                    ? (dark
                                          ? const Color(0xFF505050)
                                          : Colors.white)
                                    : Colors.transparent,
                              ),
                              boxShadow: selected && !dark
                                  ? [
                                      const BoxShadow(
                                        color: Color(0x2580CBAA),
                                        blurRadius: 15,
                                        offset: Offset(0, 5),
                                      ),
                                    ]
                                  : null,
                            ),
                            child: ExcludeSemantics(
                              child: Column(
                                mainAxisSize: MainAxisSize.min,
                                children: [
                                  Icon(
                                    destination.icon,
                                    size: 26,
                                    color: foreground,
                                  ),
                                  const SizedBox(height: 6),
                                  Text(
                                    destination.label,
                                    textAlign: TextAlign.center,
                                    softWrap: true,
                                    style: TextStyle(
                                      fontSize: 11.5,
                                      height: 1.15,
                                      fontWeight: selected
                                          ? FontWeight.w700
                                          : FontWeight.w500,
                                      color: foreground,
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
                );
              }),
            ),
          ),
        ),
      ),
    );
  }
}
