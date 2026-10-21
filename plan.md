# MASTER MISSION: RECREATE THE COMPLETE ZEN BROWSER EXPERIENCE IN THIS CHROMIUM-BASED BROWSER

You are the principal browser engineer, senior Chromium developer, frontend architect, UI/UX reverse engineer, animation engineer, and quality assurance engineer responsible for transforming the existing browser project into a comprehensive recreation of the Zen Browser experience.

Your mission is to inspect, understand, implement, and verify as much of Zen Browser's complete functionality, user experience, interface, design system, and interaction behavior as the available reference material permits.

This is NOT a request to make a browser that merely looks similar to Zen Browser.

This is NOT a request to create a basic prototype with a sidebar, tabs, and a settings page.

This is NOT a request to implement only the most obvious features.

The objective is to reproduce the complete observable experience as faithfully as technically possible, including browser layout, navigation, tab management, workspaces, panels, settings, context menus, keyboard shortcuts, toolbars, animations, icons, themes, preferences, edge cases, and the relationships between all these systems.

Treat the publicly available Zen Browser repository provided to you as a primary technical reference.

Treat the current project in this directory as the browser that must actually be improved.

Your final result must be a functioning application, not a design mockup.

---

# 1. NON-NEGOTIABLE RULES

## Rule 1: Inspect before implementing

Before changing anything:

1. Inspect the entire existing project structure.
2. Identify the programming languages, frameworks, build systems, and dependencies.
3. Determine how Chromium is integrated into the application.
4. Determine how browser windows, tabs, navigation, profiles, preferences, and native controls currently work.
5. Identify the existing UI architecture and design system.
6. Inspect the available Zen Browser repository and its history where useful.
7. Identify which parts of the existing application can be reused and which require architectural changes.
8. Establish how to build, run, and test the application.

Do not assume that this project uses a particular framework or Chromium integration model.

Do not assume that an existing feature is missing without investigating it.

Do not overwrite working functionality merely because a different implementation seems easier.

Do not begin by replacing the entire application with a new frontend.

First understand the architecture. Then make a deliberate implementation plan.

## Rule 2: The repository is a reference, not a superficial inspiration

Explore the available Zen Browser source code systematically.

Investigate its:

- Application structure.
- Browser interface.
- Tab management.
- Sidebar architecture.
- Workspace implementation.
- Preferences and settings.
- Theme system.
- Keyboard shortcuts.
- Menus and contextual actions.
- Browser chrome.
- Navigation controls.
- Panels and popovers.
- Animation behavior.
- Responsive behavior.
- Persistence mechanisms.
- Platform-specific behavior.
- Feature flags.
- Relevant CSS, stylesheets, scripts, components, resources, and configuration files.

Follow the implementation of important features through their dependencies.

When a feature appears in the interface, investigate how it actually works rather than stopping after finding its visual component.

Where possible, identify the complete chain:

USER ACTION → EVENT HANDLER → APPLICATION STATE → BROWSER OPERATION → UI UPDATE → PERSISTENCE.

If the reference repository contains a feature, investigate how it is implemented and determine the best technically compatible way to reproduce its observable behavior in this Chromium-based application.

Do not fabricate behavior when source code or executable reference behavior can be investigated.

If repository access is unavailable, identify that limitation and use the reference material actually available to you. Never pretend that you inspected files you could not access.

## Rule 3: No fake implementations

The following are unacceptable as final implementations of a feature:

- A button that does nothing.
- A menu item that only closes the menu.
- A settings toggle that changes visually but has no effect.
- A workspace selector that does not change the active workspace.
- A tab that looks selected but points to the wrong browser page.
- A theme picker that does not apply the theme.
- A search field that does not search.
- A settings page consisting of placeholder controls.
- An animation that plays without the corresponding operation occurring.
- A feature that works only until the application restarts when it is supposed to persist.
- A mock browser interface that is disconnected from Chromium.
- A fake loading indicator used to conceal a missing operation.
- A collection of unrelated components that do not share real application state.

Every implemented control must have a defined purpose and a working outcome.

If a feature cannot yet be completed, document its exact limitation and dependencies. Do not present it as finished.

## Rule 4: Preserve Chromium functionality

The application must remain a real Chromium-based browser.

Preserve or correctly implement:

- URL navigation.
- Search-engine integration.
- Back and forward navigation.
- Page loading and load failures.
- Reloading and stopping navigation.
- New tabs.
- Tab closing.
- Tab switching.
- Keyboard navigation.
- Downloads.
- Web permissions.
- Cookies and website storage.
- Browser history.
- Bookmarks.
- Private browsing, where supported by the existing architecture.
- Developer tools.
- Web content interaction.
- Profile isolation, where supported.
- Session restoration, where supported.
- Security-related browser behavior.

Investigate which capabilities already exist in the Chromium integration and which must be exposed to the interface.

Never replace actual browser operations with decorative frontend simulations.

Do not weaken Chromium security settings to make an interface feature easier to implement.

## Rule 5: Work incrementally and preserve a buildable application

Use small, coherent implementation steps.

After each significant group of changes:

1. Run relevant tests.
2. Check for compilation errors.
3. Check for type errors and lint failures when configured.
4. Verify that existing browser functionality still works.
5. Test the affected interactions.
6. Fix regressions before continuing.

Do not postpone all testing until the end.

Do not make a huge collection of unrelated changes without verifying the intermediate results.

Keep the application in a recoverable state.

Use version control if available. Do not discard user changes, reset unrelated work, or overwrite existing files without understanding their purpose.

---

# 2. PHASE ZERO: ESTABLISH THE REFERENCE AND BASELINE

Before implementation, determine the precise reference version and the current state of the target application.

## 2.1 Reference identification

Determine:

- Which Zen Browser repository is available.
- Which branch or release is being examined.
- Whether the reference represents a stable release or development version.
- Which features are present in that version.
- Whether the source code includes the relevant UI and browser integration.
- Which features depend on operating-system capabilities.
- Which features depend on external services, APIs, or optional extensions.

If Git access is available, inspect the current reference revision and relevant history.

Do not automatically assume that the latest development branch is the correct reference if the supplied repository corresponds to a different version.

## 2.2 Target application baseline

Determine:

- How the application starts.
- How it builds.
- How its browser engine is embedded or launched.
- How the UI communicates with Chromium.
- How state is managed.
- How preferences are stored.
- How native menus and dialogs are created.
- How icons and other assets are loaded.
- How browser windows and web contents are managed.
- Which tests already exist.
- Which features are already working.

Run the existing application if the environment allows it.

Record current errors before changing the code.

## 2.3 Feature inventory

Create a persistent project document:

`docs/zen-parity/FEATURE_INVENTORY.md`

For every discoverable reference feature, record:

- Feature name.
- Reference source files or evidence.
- User-visible behavior.
- Relevant application state.
- Required browser-engine integration.
- Related settings.
- Related menus and shortcuts.
- Persistence requirements.
- Platform dependencies.
- Current target implementation.
- Missing work.
- Testing requirements.
- Implementation status.

Use the following statuses:

- NOT INVESTIGATED
- INVESTIGATED
- NOT IMPLEMENTED
- PARTIALLY IMPLEMENTED
- IMPLEMENTED
- TESTED
- BLOCKED
- NOT APPLICABLE

Never mark a feature TESTED merely because the code compiles.

Keep this inventory updated throughout the entire mission.

---

# 3. PHASE ONE: REVERSE-ENGINEER THE COMPLETE USER INTERFACE

The interface must be studied as a complete system.

Do not investigate only the main browser window.

## 3.1 Main browser window

Inspect and reproduce, as applicable to the reference:

- Overall window composition.
- Window borders.
- Title bar.
- Native window controls.
- Browser toolbar.
- Sidebar.
- Tab area.
- Navigation controls.
- Address bar.
- Search behavior.
- Bookmark controls.
- Extension controls.
- Main web-content region.
- Bottom bars.
- Status indicators.
- Popovers.
- Overlays.
- Empty states.
- Loading states.
- Error states.
- Window resizing.
- Full-screen behavior.
- Maximized and restored window states.

Investigate how every region behaves when the window dimensions change.

Do not rely on a single screenshot as the complete specification.

## 3.2 Sidebar and navigation layout

Investigate every available sidebar configuration.

Determine the reference behavior for:

- Expanded sidebar.
- Collapsed sidebar.
- Compact sidebar.
- Vertical tab presentation.
- Sidebar width.
- Sidebar resizing.
- Sidebar visibility.
- Sidebar positioning.
- Sidebar persistence.
- Sidebar hover behavior.
- Sidebar activation.
- Sidebar transition timing.
- Sidebar background and transparency.
- Sidebar content spacing.
- Sidebar scroll behavior.
- Sidebar overflow.
- Sidebar controls.
- Sidebar context menus.
- Sidebar behavior during full-screen mode.
- Sidebar behavior with different window sizes.

Identify the difference between persistent controls, temporary controls, hover-revealed controls, and context-sensitive controls.

Reproduce these distinctions instead of displaying every control permanently.

## 3.3 Tabs

Investigate the entire tab system.

Where supported by the reference, implement:

- Vertical tabs.
- Tab selection.
- Active-tab styling.
- Inactive-tab styling.
- New tabs.
- Tab closing.
- Tab duplication.
- Tab reordering.
- Drag-and-drop.
- Tab pinning.
- Tab muting.
- Tab loading indicators.
- Tab favicons.
- Tab titles.
- Tab tooltips.
- Tab context menus.
- Tab keyboard navigation.
- Tab creation from links.
- Tab creation from the address bar.
- Tab restoration.
- Recently closed tabs.
- Tab grouping.
- Tab nesting.
- Parent-child tab relationships.
- Collapsible tab groups.
- Pinned tab regions.
- Tab overflow.
- Tab scrolling.
- Tab context actions.
- Tab-specific settings.
- Tab-specific visual states.
- Tab drag indicators.
- Tab drop targets.
- Tab closing animations.
- Tab creation animations.
- Tab movement animations.
- Tab selection animations.

Do not implement every item blindly. Verify which behaviors exist in the reference version, then reproduce the relevant ones.

Ensure that the UI's tab state corresponds to real Chromium pages or the appropriate underlying browser objects.

Closing a tab must close the correct underlying page.

Switching a tab must activate the correct web contents.

Moving a tab must update the actual tab model, not just the visual ordering.

## 3.4 Workspaces and profiles

Investigate the reference's workspace model and distinguish it from browser profiles.

Where supported, implement:

- Workspace creation.
- Workspace deletion.
- Workspace renaming.
- Workspace switching.
- Workspace icons.
- Workspace colors.
- Workspace-specific tab collections.
- Workspace-specific tab restoration.
- Workspace ordering.
- Workspace switching animations.
- Workspace context menus.
- Workspace persistence.
- Empty workspaces.
- Switching between workspaces with many tabs.
- Behavior when the active tab is removed.
- Behavior when the active workspace is removed.
- Behavior when the application restarts.

Determine whether workspaces share the same browser profile or use isolated browser contexts.

Do not accidentally imply that workspaces provide privacy or storage isolation when they do not.

If profile separation exists in the reference, investigate its storage, cookie, session, and permission implications separately.

## 3.5 Additional tab and navigation concepts

Investigate all reference-specific features that affect browsing organization.

These may include:

- Split views.
- Multiple simultaneous web pages.
- Tab containers.
- Compact tab modes.
- Temporary tabs.
- Essential or persistent tabs.
- Tab suspension.
- Tab search.
- Tab previews.
- Context-specific navigation.
- Special browsing modes.
- Custom new-tab experiences.

For each feature, determine its actual semantics before implementing it.

Do not create a superficially similar feature that behaves differently in important ways.

---

# 4. PHASE TWO: SETTINGS AND PREFERENCES COMPLETENESS

Settings are a major part of this mission.

Do not create a generic settings screen and call it complete.

Inspect the actual settings implementation in the reference repository.

Create:

`docs/zen-parity/SETTINGS_INVENTORY.md`

List every settings category, subcategory, preference, toggle, selector, slider, text field, action button, and reset mechanism that is present in the examined reference version.

## 4.1 Settings categories

Investigate all categories that exist in the reference, including relevant categories such as:

- General.
- Appearance.
- Theme.
- Tabs.
- Workspaces.
- Sidebar.
- Navigation.
- Address bar.
- Search.
- Privacy.
- Security.
- Downloads.
- Bookmarks.
- Keyboard shortcuts.
- Extensions.
- Website permissions.
- Language.
- Accessibility.
- Advanced settings.
- Developer options.
- Updates.
- Import and export.
- Browser data.
- Profiles.
- Experimental features.

This is an investigation list, not permission to invent settings that the reference does not have.

Discover the actual categories and reproduce their behavior.

## 4.2 Every setting must work

For each preference, document:

1. Its name.
2. Its description.
3. Its default value.
4. Its accepted values.
5. Its dependencies.
6. Its immediate effects.
7. Whether it requires a restart.
8. Where it is persisted.
9. Whether it applies to the current window, current profile, or entire application.
10. How it is tested.

Every control must be connected to a real preference or operation.

If a setting changes the interface, the interface must actually respond.

If a setting affects browser behavior, the underlying browser integration must apply it.

If a setting is persisted, verify that it survives an application restart.

If a setting depends on an unavailable capability, communicate that limitation rather than displaying a misleading control.

## 4.3 Settings interface

Reproduce the settings interface's:

- Navigation structure.
- Page layout.
- Section spacing.
- Typography.
- Control sizing.
- Labels.
- Descriptions.
- Search behavior.
- Focus behavior.
- Hover states.
- Active states.
- Disabled states.
- Toggle animations.
- Dropdown menus.
- Sliders.
- Confirmation dialogs.
- Reset buttons.
- Warning messages.
- Save behavior.
- Unsaved-change handling.
- Scroll behavior.
- Keyboard accessibility.
- Responsive behavior.

Study how settings pages are opened, navigated, closed, and restored.

Determine whether settings appear inside a browser tab, a separate window, a panel, or another interface.

Reproduce the actual reference behavior where technically possible.

---

# 5. PHASE THREE: MENUS, COMMANDS, AND INTERACTIONS

Investigate every interactive element in the reference.

This includes controls that are not visible until the user hovers, right-clicks, opens a submenu, or activates a keyboard shortcut.

## 5.1 Context menus

Inspect and implement the applicable context menus for:

- Tabs.
- Pinned tabs.
- Tab groups.
- Workspaces.
- Sidebar items.
- Navigation controls.
- Address bar.
- Web pages.
- Links.
- Images.
- Selected text.
- Bookmarks.
- Downloads.
- Toolbar buttons.
- Browser windows.
- Settings controls.

For each menu, reproduce:

- Available commands.
- Command ordering.
- Separators.
- Icons.
- Labels.
- Submenus.
- Checkmarks.
- Disabled states.
- Keyboard accelerators.
- Hover behavior.
- Positioning.
- Dismissal behavior.
- Opening and closing animations.

Every command must execute the correct operation.

## 5.2 Command palette and search

If present in the reference, investigate:

- Opening shortcuts.
- Searchable commands.
- Command categories.
- Search ranking.
- Keyboard navigation.
- Selection.
- Command execution.
- Recent commands.
- Empty results.
- Result highlighting.
- Focus management.
- Dismissal behavior.

Do not confuse a command palette with address-bar search or tab search. Implement their distinct behaviors.

## 5.3 Keyboard shortcuts

Extract the available shortcuts from source code and configuration.

Create:

`docs/zen-parity/KEYBOARD_SHORTCUTS.md`

For each shortcut, document:

- Default key combination.
- Command.
- Context.
- Platform differences.
- Whether it is configurable.
- Conflicts with existing browser or operating-system shortcuts.
- Whether it is implemented.
- How it is tested.

Implement the applicable shortcuts and make sure they invoke the same operations as their corresponding UI controls.

Do not hardcode conflicting shortcuts without understanding the platform and browser integration.

## 5.4 Interaction matrix

Build an interaction matrix covering every visible control.

For each control, record:

- Default state.
- Hover state.
- Focus state.
- Active state.
- Disabled state.
- Loading state.
- Error state.
- Click behavior.
- Double-click behavior, if applicable.
- Keyboard behavior.
- Context-menu behavior.
- Drag behavior.
- Dismissal behavior.
- Persistence behavior.

Do not add meaningless states to controls that do not need them. Match the actual behavior of the reference.

---

# 6. PHASE FOUR: DESIGN SYSTEM AND VISUAL FIDELITY

The visual implementation must be deliberate, consistent, and grounded in the reference.

Do not approximate the entire interface with arbitrary CSS values.

## 6.1 Inspect the actual design system

Where available, identify:

- Design tokens.
- CSS variables.
- Theme definitions.
- Color palettes.
- Spacing scales.
- Typography.
- Font weights.
- Font sizes.
- Line heights.
- Border radii.
- Border widths.
- Shadows.
- Blur effects.
- Transparency.
- Surface hierarchy.
- Component dimensions.
- Icon sizes.
- Focus rings.
- Transition curves.
- Transition durations.
- Responsive breakpoints.
- Density settings.

Extract these into a coherent design-system implementation.

Avoid scattering unrelated hardcoded values across dozens of components.

Use centralized tokens wherever practical.

## 6.2 Color and surface matching

Investigate the exact behavior of:

- Main background.
- Sidebar background.
- Active tab.
- Inactive tab.
- Hovered tab.
- Selected item.
- Floating panels.
- Menus.
- Dialogs.
- Address bar.
- Search field.
- Borders.
- Separators.
- Shadows.
- Focus outlines.
- Disabled controls.
- Warning states.
- Error states.
- Accent colors.
- Light theme.
- Dark theme.
- System theme.
- Custom themes.

Where the reference supports theme variants, reproduce their observable differences.

Pay attention to transparency, layering, contrast, and the visual relationship between neighboring surfaces.

Do not assume that changing a single background color reproduces a theme.

## 6.3 Typography and spacing

Measure or derive:

- Sidebar padding.
- Tab spacing.
- Toolbar height.
- Address bar height.
- Icon alignment.
- Text baselines.
- Label spacing.
- Button dimensions.
- Menu row height.
- Dialog spacing.
- Section spacing.
- Content margins.
- Scrollbar appearance.

Avoid accumulating arbitrary margin and padding overrides.

Fix the layout at the component or design-token level.

## 6.4 Icons

Inventory every icon used by the reference interface.

Determine:

- Which icon is used.
- Its dimensions.
- Its stroke width.
- Its fill behavior.
- Its color.
- Its alignment.
- Its hover state.
- Its active state.
- Its disabled state.
- Whether it changes with the current theme.
- Whether it animates.
- Whether it is platform-specific.

Use suitable licensed icons, original vector implementations, or other assets that the project is permitted to use.

Do not replace an entire icon system with generic Unicode characters or emoji.

Do not assume that two visually similar icons are interchangeable if their meaning differs.

Keep icon sizing and alignment consistent.

## 6.5 Pixel-level comparison

Where a runnable reference and screenshot capture are available:

1. Capture the reference at a known window size.
2. Capture the target application at the same size.
3. Match the initial state, theme, and sidebar configuration.
4. Compare the major layout boundaries.
5. Compare spacing and component dimensions.
6. Compare text alignment and typography.
7. Compare colors and transparency.
8. Compare icon sizes and positions.
9. Compare border radii and shadows.
10. Identify and fix the largest visible discrepancies.
11. Repeat the comparison after changes.

Use screenshots, image-difference tools, or other available visual inspection methods.

Do not claim pixel-perfect fidelity without performing an actual comparison.

If the reference cannot be executed in the current environment, record the limitation and use the available source code and reference screenshots.

---

# 7. PHASE FIVE: ANIMATIONS AND MOTION

Animation is part of the product behavior, not a decorative afterthought.

Investigate every significant transition in the reference.

Potential areas include:

- Sidebar expansion.
- Sidebar collapse.
- Tab creation.
- Tab removal.
- Tab reordering.
- Tab selection.
- Tab-group expansion.
- Workspace switching.
- Panel opening.
- Panel closing.
- Popovers.
- Context menus.
- Dialogs.
- Settings controls.
- Theme changes.
- Hover-revealed buttons.
- Loading indicators.
- Progress indicators.
- Drag-and-drop.
- Window transitions.
- Split-view resizing.
- Toolbar changes.
- Focus changes.

For each transition, identify:

- Trigger.
- Starting state.
- Ending state.
- Animated properties.
- Duration.
- Easing curve.
- Delay.
- Interruption behavior.
- Reversal behavior.
- Reduced-motion behavior.

Reproduce the motion characteristics as closely as the evidence permits.

Avoid introducing animation where the reference has none.

Avoid using arbitrary long transitions to imitate a polished interface.

Animations must remain responsive when the user clicks repeatedly or changes state rapidly.

Prevent animation from causing layout instability, flickering, broken hitboxes, or incorrect component positioning.

Respect operating-system and application reduced-motion preferences when applicable.

Create:

`docs/zen-parity/ANIMATION_INVENTORY.md`

Include all investigated transitions and their implementation status.

---

# 8. PHASE SIX: BROWSER ENGINE INTEGRATION

A visually accurate browser that cannot correctly control Chromium is not an acceptable result.

Investigate the existing browser-engine integration before selecting implementation strategies.

Depending on the project's architecture, this may involve Electron, CEF, Qt WebEngine, a custom Chromium shell, or another embedding model.

Do not assume that APIs available in one architecture exist in another.

## 8.1 Establish a reliable browser abstraction

Where appropriate, create or improve a browser integration layer that separates UI concerns from engine-specific operations.

Potential responsibilities include:

- Creating browser pages.
- Destroying browser pages.
- Activating pages.
- Navigating to URLs.
- Loading search queries.
- Reloading pages.
- Stopping navigation.
- Going backward and forward.
- Tracking loading state.
- Tracking page titles.
- Tracking favicons.
- Managing permissions.
- Managing downloads.
- Managing browser history.
- Managing bookmarks.
- Managing sessions.
- Managing page events.
- Managing developer tools.
- Managing window-level browser operations.

Adapt this abstraction to the existing architecture instead of forcing an unnecessary rewrite.

## 8.2 State consistency

Establish a clear source of truth for browser tabs and related application state.

Avoid maintaining multiple contradictory versions of:

- Active tab.
- Tab order.
- Tab loading state.
- Tab title.
- Tab URL.
- Pinned state.
- Workspace membership.
- Sidebar visibility.
- Theme selection.
- Window layout.
- Preferences.

When the underlying browser changes state, update the interface.

When the user changes state through the interface, update the underlying browser.

Handle unexpected page destruction, navigation failures, and asynchronous operations safely.

## 8.3 Security

Preserve the appropriate Chromium security model.

Investigate:

- Renderer-process isolation.
- Inter-process communication.
- Navigation restrictions.
- Permission requests.
- Download handling.
- External protocol handling.
- File access.
- Web-content isolation.
- Preload scripts, if applicable.
- Privileged API exposure.
- Session boundaries.
- Profile boundaries.

Do not expose privileged application APIs directly to untrusted websites.

Do not disable sandboxing, web security, or origin protections merely to simplify development.

## 8.4 Native and operating-system integration

Investigate features whose behavior depends on the operating system, including:

- Window controls.
- Window dragging.
- Native menus.
- Native file dialogs.
- System theme detection.
- System notifications.
- Global shortcuts.
- Full-screen behavior.
- Multi-monitor behavior.
- Display scaling.
- Touchpad and mouse gestures.

Implement supported behavior properly and document platform-specific differences.

---

# 9. PHASE SEVEN: PERSISTENCE AND RECOVERY

A browser must maintain a coherent experience across restarts.

Investigate the reference's persistence model and reproduce the applicable behavior.

Potential persistent state includes:

- Theme.
- Appearance preferences.
- Sidebar configuration.
- Window dimensions.
- Window position.
- Workspace configuration.
- Workspace ordering.
- Tab ordering.
- Pinned tabs.
- Tab-group configuration.
- Bookmarks.
- History.
- Downloads.
- Search-engine selection.
- Keyboard shortcuts.
- Browser preferences.
- Recently closed tabs.
- Session state.

Distinguish application preferences from browser profile data and transient UI state.

Use appropriate storage mechanisms for the existing architecture.

Do not store sensitive browser data in an insecure or unnecessarily exposed location.

Handle corrupt, missing, outdated, or partially written state gracefully.

Investigate migration requirements when a preference schema changes.

Test startup after:

- A normal shutdown.
- A forced shutdown, where safely testable.
- A missing preferences file.
- An invalid preferences file.
- A partially restored session.
- An unavailable or deleted workspace.
- A closed active tab.
- A changed display configuration.

Never silently discard recoverable user data.

---

# 10. PHASE EIGHT: ADDITIONAL FEATURES AND EDGE CASES

Do not limit the investigation to the main happy path.

Inspect the complete reference repository for additional features, experimental features, optional modules, and relevant integrations.

Depending on the actual reference version, investigate:

- Extensions.
- Bookmark management.
- History management.
- Download management.
- Private browsing.
- Search engines.
- URL-bar suggestions.
- Browser data clearing.
- Permission management.
- Reader or focus modes.
- Picture-in-picture.
- Translation.
- Spell checking.
- Find in page.
- Web capture.
- Import and export.
- Profile management.
- Accessibility features.
- Developer tools.
- Error pages.
- Network error handling.
- Crash recovery.
- Update mechanisms.
- New-tab customization.
- Custom backgrounds.
- Search shortcuts.
- External protocol handling.
- Dragging links into tabs.
- Dropping URLs into the browser.
- Opening files.
- Opening links from external applications.

For each feature, determine whether it is present in the actual reference version and whether the existing Chromium architecture can support it.

Implement the applicable features in priority order, preserving the core browsing experience.

Do not invent reference functionality based only on the name of a feature.

---

# 11. PHASE NINE: ACCESSIBILITY AND INPUT METHODS

Investigate and implement the applicable accessibility behaviors.

Check:

- Keyboard-only navigation.
- Focus visibility.
- Focus order.
- Screen-reader labels.
- Semantic controls.
- Accessible names.
- Dialog focus management.
- Escape-key behavior.
- Tab navigation.
- High-contrast behavior.
- Reduced-motion preferences.
- Text scaling.
- Tooltip accessibility.
- Color contrast.
- Pointer target sizes.
- Keyboard alternatives to drag operations.

Also investigate supported input methods, including:

- Mouse.
- Trackpad.
- Touch, where supported.
- Mouse wheel.
- Middle-click.
- Right-click.
- Keyboard modifiers.
- Drag-and-drop.
- Operating-system shortcuts.

The application must remain usable, not merely visually convincing.

---

# 12. PHASE TEN: TESTING AND VERIFICATION

Testing is mandatory.

Create a structured test plan based on the discovered reference functionality.

## 12.1 Core browser tests

Verify:

- Launching the application.
- Opening a URL.
- Performing a search.
- Navigating backward and forward.
- Reloading.
- Stopping navigation.
- Creating tabs.
- Switching tabs.
- Closing tabs.
- Reordering tabs.
- Pinning tabs, if supported.
- Opening and closing workspaces.
- Changing themes.
- Changing sidebar configuration.
- Opening settings.
- Updating preferences.
- Restarting and verifying persistence.
- Handling navigation failures.
- Handling unexpected page closure.

## 12.2 UI tests

Verify:

- Buttons.
- Menus.
- Submenus.
- Popovers.
- Dialogs.
- Tooltips.
- Focus behavior.
- Hover behavior.
- Disabled states.
- Keyboard shortcuts.
- Animations.
- Drag-and-drop.
- Window resizing.
- Sidebar transitions.
- Theme transitions.
- Settings navigation.

## 12.3 Regression tests

Ensure that new functionality does not break:

- Existing browser navigation.
- Existing browser storage.
- Security boundaries.
- Tab switching.
- Window management.
- Application startup.
- Application shutdown.
- Previously implemented features.

## 12.4 Automated tests

Use the testing infrastructure already present in the project.

Add suitable tests where practical.

Potential test types include:

- Unit tests.
- Component tests.
- Integration tests.
- Browser-operation tests.
- End-to-end tests.
- Screenshot comparisons.
- Persistence tests.
- Keyboard interaction tests.

Do not add a new testing framework without first checking whether the existing project already has one.

## 12.5 Manual verification

When GUI interaction is available, launch the application and exercise the real interface.

Do not rely exclusively on source-code inspection.

Do not claim a feature works because its event handler exists.

Test the actual outcome.

When GUI interaction is unavailable, perform the strongest alternative verification possible and explicitly document what could not be tested.

---

# 13. IMPLEMENTATION STRATEGY

Implement the project in dependency-aware stages.

## Stage A: Discovery

- Inspect both repositories.
- Establish the current architecture.
- Identify reference features.
- Build the inventories.
- Identify build and test commands.
- Record existing defects.

Deliverable: a documented baseline and implementation roadmap.

## Stage B: Foundation

- Establish reliable application state.
- Establish browser integration boundaries.
- Establish design tokens.
- Establish reusable UI components.
- Establish persistence mechanisms.
- Establish an interaction and event-handling strategy.

Deliverable: a stable foundation for implementing the remaining features.

## Stage C: Core browser interface

- Main browser window.
- Sidebar.
- Tabs.
- Navigation toolbar.
- Address bar.
- Main content area.
- Core menus.
- Window behavior.

Deliverable: a cohesive, functioning primary browsing experience.

## Stage D: Advanced tab and workspace functionality

- Workspace management.
- Tab organization.
- Tab context menus.
- Supported tab groups.
- Advanced navigation interactions.
- Associated persistence.

Deliverable: the applicable advanced organizational features found in the reference.

## Stage E: Settings and customization

- Complete settings inventory.
- Appearance controls.
- Theme system.
- Browser preferences.
- Keyboard shortcuts.
- Persistence.
- Reset and migration behavior.

Deliverable: working settings rather than decorative controls.

## Stage F: Visual fidelity

- Icons.
- Typography.
- Spacing.
- Colors.
- Component dimensions.
- Hover states.
- Shadows.
- Borders.
- Responsive behavior.
- Reference-based screenshot comparisons.

Deliverable: a visually faithful interface based on the available evidence.

## Stage G: Animations and interaction polish

- Transition inventory.
- Easing and timing.
- Hover-revealed controls.
- Tab transitions.
- Sidebar transitions.
- Workspace transitions.
- Menus and dialogs.
- Reduced-motion behavior.

Deliverable: polished, coherent interactions.

## Stage H: Remaining reference features

- Additional discovered functionality.
- Browser integrations.
- Edge cases.
- Platform-specific features.
- Relevant experimental features.

Deliverable: the broadest feasible feature parity with the selected reference version.

## Stage I: Verification

- Build.
- Test.
- Compare screenshots.
- Test persistence.
- Test keyboard interactions.
- Resolve regressions.
- Audit unimplemented controls.
- Update all inventories.

Deliverable: a tested implementation with explicit limitations.

Do not stop after completing Stage C.

Do not interpret completing one stage as permission to abandon the remaining stages.

Continue until the relevant inventory has been addressed or a genuine technical blocker has been documented.

---

# 14. HOW TO WORK AUTONOMOUSLY

Operate as an engineer responsible for delivering the implementation.

Do not ask for permission before every routine engineering decision.

Inspect the code, determine the correct approach, and proceed when the decision is reasonably supported by the repository.

If multiple approaches are possible, prefer the one that:

1. Preserves existing working functionality.
2. Matches the reference behavior.
3. Fits the existing architecture.
4. Maintains Chromium security.
5. Minimizes unnecessary dependencies.
6. Remains maintainable.
7. Can be tested reliably.

If you discover a major architectural limitation, investigate alternatives before abandoning the feature.

If an implementation requires a dependency, first check whether an existing dependency already solves the problem.

Do not install arbitrary packages merely to avoid understanding the current architecture.

Do not create a second competing implementation of a feature that already exists.

Do not introduce unnecessary abstractions.

Do not rewrite stable systems without a clear reason.

Keep a persistent engineering log at:

`docs/zen-parity/IMPLEMENTATION_LOG.md`

After each significant implementation stage, record:

- What was investigated.
- What was implemented.
- Which files changed.
- Which tests were executed.
- Which tests passed.
- Which tests failed.
- Which problems were fixed.
- Which problems remain.
- What should happen next.

Maintain a clear next-action list so work can continue after context limits or interrupted sessions.

---

# 15. REQUIRED COMPLETION AUDIT

Before considering the mission complete, audit the application against the reference.

Inspect the complete feature inventory, settings inventory, shortcut inventory, animation inventory, and interaction matrix.

For every feature, ask:

1. Does the feature exist in the reference version?
2. Has its implementation been investigated?
3. Is the corresponding target functionality implemented?
4. Does the UI reflect the real application state?
5. Does the underlying browser operation work?
6. Does persistence work where required?
7. Do the associated menus and keyboard shortcuts work?
8. Do the relevant animations and visual states work?
9. Have edge cases been considered?
10. Has the feature been tested?
11. Is the feature subject to a platform or architecture limitation?
12. Is the status recorded accurately?

Search the target codebase for:

- TODO markers.
- Placeholder implementations.
- Empty event handlers.
- Buttons without actions.
- Hardcoded demonstration data.
- Temporary mock state.
- Disabled controls.
- Incomplete settings.
- Unused components.
- Duplicate state.
- Unhandled errors.
- Missing persistence.
- Unimplemented menu actions.

Investigate every relevant result.

Not every TODO or disabled control is necessarily a defect, but none should be ignored without examination.

Do not remove legitimate error handling or security restrictions just to make the audit appear clean.

---

# 16. DEFINITION OF DONE

The mission is complete only when all of the following are true to the extent supported by the selected reference version and the target architecture:

- The application builds successfully.
- The application launches successfully.
- Core Chromium browsing works.
- The primary browser layout closely matches the reference.
- The applicable tab-management functionality works.
- The applicable workspace functionality works.
- The relevant menus and contextual actions work.
- The settings inventory has been addressed.
- Implemented settings produce real effects.
- Persistent preferences survive restart.
- The relevant keyboard shortcuts work.
- Icons and visual states are consistent.
- The applicable animations have been implemented.
- Browser state and UI state remain synchronized.
- Important edge cases have been addressed.
- Existing functionality has not been silently broken.
- Security has not been weakened to bypass implementation challenges.
- Relevant automated tests pass.
- Manual verification has been performed where possible.
- Remaining limitations are documented.
- All inventories reflect the actual implementation status.

Do not describe the application as a complete recreation if substantial reference features remain unimplemented.

Do not confuse visual similarity with functional parity.

Do not confuse a successful build with a successful product.

Do not mark untested features as tested.

---

# 17. YOUR FIRST ACTIONS

Begin immediately.

First, inspect the current working directory and identify the application architecture.

Then inspect the available Zen Browser repository.

Determine the reference version and the target application's current capabilities.

Create the feature inventories and implementation roadmap.

Identify the highest-impact architectural gaps.

Begin implementing the project in the dependency-aware order described above.

After the initial investigation, give me a concise report containing:

1. The current browser architecture.
2. The Zen Browser reference version examined.
3. The major systems already implemented in my browser.
4. The major systems missing from my browser.
5. Any architectural blockers.
6. The implementation plan.
7. The first concrete changes you are going to make.

Then continue working.

Do not stop after producing the report.

Do not merely tell me what needs to be done.

Actually inspect the code, implement the features, run the tests, fix the failures, and keep progressing through the inventories.

If you reach a context limit, leave the project in a coherent state and update the implementation log with the exact next steps.

When work resumes, read the inventories and implementation log, inspect the current code, verify the existing state, and continue from the next unfinished task.

The objective is a complete, functioning, carefully verified recreation of the applicable Zen Browser experience within the technical and licensing constraints of this Chromium-based project.

BEGIN WITH REPOSITORY DISCOVERY.