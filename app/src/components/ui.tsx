// Shared primitives. Tailwind only sees literal class strings, so variants
// are spelled out in full maps rather than built by concatenation.

import { useRef, useState } from 'react';
import { ActivityIndicator, Platform, Pressable, RefreshControl, ScrollView, Text, TextProps, useWindowDimensions, View, ViewProps } from 'react-native';
import { router } from 'expo-router';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

import { Severity } from '@/protocol/comfort';
import { useTheme } from '@/theme';

// Display text (titles, big numbers) is already large: past 1.4× it breaks
// words in half at accessibility sizes. Body text scales without a cap.
const DISPLAY_FONT = /\bfont-(wordmark|title|number)/;

export function T({ className = '', ...props }: TextProps & { className?: string }) {
  const cap = DISPLAY_FONT.test(className) ? 1.4 : undefined;
  return <Text className={`font-body text-ink ${className}`} maxFontSizeMultiplier={cap} {...props} />;
}

export function Screen({
  children,
  scroll = true,
  onRefresh,
  refreshing = false,
  tabBar = false,
}: {
  children: React.ReactNode;
  scroll?: boolean;
  /** A tab screen: the floating tab bar covers the bottom, so the end scrolls clear of it. */
  tabBar?: boolean;
  /** Pull to refresh, when given. */
  onRefresh?: () => void;
  refreshing?: boolean;
}) {
  const t = useTheme();
  const devScroll = useRef<ScrollView>(null);
  const insets = useSafeAreaInsets();
  // The bottom clears the home indicator, and on an iOS tab screen the tab
  // bar, which floats about 50 pt above it. Android's tab bar takes its own
  // space below the screen.
  const bottom = insets.bottom + (tabBar && Platform.OS === 'ios' ? 72 : 16);
  if (!scroll) {
    return (
      <View className="flex-1 bg-bg px-4" style={{ paddingTop: insets.top }}>
        {children}
      </View>
    );
  }
  // A View padded in JS, not SafeAreaView: SafeAreaView pads natively, and the
  // ScrollView inside kept the full screen height, so its last insets.top
  // points hung below the screen where no scroll could reach them.
  return (
    <View className="flex-1 bg-bg" style={{ paddingTop: insets.top }}>
      <ScrollView
        // TEMP (UI review in the simulator): open scrolled to the end when devScrollEnd is set.
        onContentSizeChange={__DEV__ ? () => (globalThis as { __devScrollEnd?: boolean }).__devScrollEnd && devScroll.current?.scrollToEnd({ animated: false }) : undefined}
        ref={devScroll}
        contentContainerClassName="px-4 gap-4"
        contentContainerStyle={{ paddingBottom: bottom }}
        // Insets are explicit (top above, paddingBottom below), so iOS adds none of its own.
        contentInsetAdjustmentBehavior="never"
        refreshControl={onRefresh ? <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={t.textMuted} /> : undefined}>
        {children}
      </ScrollView>
    </View>
  );
}

export function Title({ children, detail }: { children: React.ReactNode; detail?: React.ReactNode }) {
  return (
    <View className="flex-row items-end justify-between pt-3 pb-1 gap-3">
      {/* Beside a detail (a unit chip), the title is a short word and the chip gives way;
          alone (a unit's name), it wraps rather than being cut. */}
      <T className={`font-wordmark text-[30px] leading-[38px] ${detail ? 'shrink-0' : 'shrink'}`} numberOfLines={detail ? 1 : 2}>
        {children}
      </T>
      {detail}
    </View>
  );
}

/**
 * A modal screen. iOS usually presents it as a sheet below the status bar, but
 * not always (replacing one modal with another can present it full screen);
 * Android always presents it full screen. So it clears the status bar only when
 * it turns out to fill the window.
 */
export function Sheet({ children, className = '', tone = 'bg' }: { children: React.ReactNode; className?: string; tone?: 'bg' | 'surface' }) {
  const insets = useSafeAreaInsets();
  const { height: windowHeight } = useWindowDimensions();
  // A sheet is shorter than the window; full screen, it is as tall as the window.
  const [fullScreen, setFullScreen] = useState(Platform.OS === 'android');
  return (
    <View
      onLayout={(e) => setFullScreen(e.nativeEvent.layout.height >= windowHeight - 1)}
      style={{ paddingTop: fullScreen ? insets.top : 0 }}
      className={`flex-1 ${tone === 'surface' ? 'bg-surface' : 'bg-bg'}`}>
      <View className={`flex-1 ${className}`}>{children}</View>
    </View>
  );
}

/**
 * Title text with subscript digits (CO₂) drawn in the body font: Kiwi Maru's
 * own are tiny and hang below the baseline.
 */
export function TitleText({ children }: { children: string }) {
  const parts = children.split(/([₀-₉]+)/);
  if (parts.length === 1) return <>{children}</>;
  return (
    <>
      {parts.map((part, i) =>
        i % 2 ? (
          <T key={i} className="font-body-medium">
            {part}
          </T>
        ) : (
          part
        ),
      )}
    </>
  );
}

/** A modal's title row: an optional eyebrow, the title, and one text action on the right. */
export function SheetHeader({ title, eyebrow, action, onAction }: { title: string; eyebrow?: string; action: string; onAction: () => void }) {
  return (
    <View className="gap-1">
      {eyebrow && <T className="font-body-semi text-muted text-xs uppercase tracking-widest">{eyebrow}</T>}
      <View className="flex-row justify-between items-center gap-4">
        <T className="font-title text-2xl leading-9 shrink">
          <TitleText>{title}</TitleText>
        </T>
        <Pressable onPress={onAction} accessibilityRole="button" hitSlop={12}>
          <T className="text-primary font-body-semi">{action}</T>
        </Pressable>
      </View>
    </View>
  );
}

export function Card({ className = '', ...props }: ViewProps & { className?: string }) {
  return <View className={`bg-surface rounded-card p-4 ${className}`} {...props} />;
}

export function SectionLabel({ children }: { children: React.ReactNode }) {
  return <T className="font-body-semi text-muted text-xs uppercase tracking-widest mt-3 px-1">{children}</T>;
}

/** `quiet` is a text-only button for the lesser of two choices. */
type ButtonKind = 'primary' | 'secondary' | 'danger' | 'quiet';

const BUTTON: Record<ButtonKind, { box: string; text: string }> = {
  primary: { box: 'bg-primary', text: 'text-on-primary' },
  secondary: { box: 'bg-raised', text: 'text-ink' },
  danger: { box: 'bg-bad/15', text: 'text-bad' },
  quiet: { box: '', text: 'text-primary' },
};

export function Button({
  title,
  onPress,
  kind = 'primary',
  busy,
  disabled,
}: {
  title: string;
  onPress: () => void;
  kind?: ButtonKind;
  busy?: boolean;
  disabled?: boolean;
}) {
  const t = useTheme();
  const style = BUTTON[kind];
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ disabled: !!(disabled || busy), busy: !!busy }}
      onPress={onPress}
      disabled={disabled || busy}
      // A minimum height, not a fixed one: at large text sizes the label grows the button.
      className={`${style.box} min-h-12 py-2 rounded-full flex-row items-center justify-center gap-2 px-5 active:opacity-80 ${disabled ? 'opacity-40' : ''}`}>
      {busy && <ActivityIndicator color={kind === 'primary' ? t.onPrimary : t.text} />}
      <T className={`font-body-semi text-base text-center shrink ${style.text}`}>{title}</T>
    </Pressable>
  );
}

const PILL_DOT: Record<'ok' | 'warn' | 'bad' | 'off', string> = {
  ok: 'bg-primary',
  warn: 'bg-warn',
  bad: 'bg-bad',
  off: 'bg-muted',
};

export function Pill({ tone, children }: { tone: 'ok' | 'warn' | 'bad' | 'off'; children: React.ReactNode }) {
  return (
    <View className="flex-row items-center gap-1.5 bg-surface rounded-full px-2.5 py-1 shrink">
      <View className={`w-1.5 h-1.5 rounded-full ${PILL_DOT[tone]}`} />
      <T className="font-body-semi text-[11px] text-muted shrink">{children}</T>
    </View>
  );
}

/** Literal class sets per verdict, so Tailwind keeps them. */
export const SEVERITY_CLASS: Record<Severity, { text: string; soft: string }> = {
  [Severity.Ok]: { text: 'text-primary', soft: 'bg-primary/10' },
  [Severity.Warn]: { text: 'text-warn', soft: 'bg-warn/15' },
  [Severity.Bad]: { text: 'text-bad', soft: 'bg-bad/15' },
};

export function Row({
  label,
  value,
  onPress,
  last,
  danger,
}: {
  label: string;
  value?: React.ReactNode;
  onPress?: () => void;
  last?: boolean;
  /** Destructive action: red label, no chevron. */
  danger?: boolean;
}) {
  // At the largest text sizes one line can't hold both without splitting
  // words, so the value goes under the label.
  const stacked = useWindowDimensions().fontScale > 2;
  const content = (
    <View
      className={`${stacked ? 'flex-col items-start gap-1' : 'flex-row items-center justify-between gap-4'} py-3.5 ${last ? '' : 'border-b border-line'}`}>
      {/* Label and value share the width, so at large text sizes neither is
          squeezed until a word breaks in half. */}
      <T className={`text-[15px] shrink ${danger ? 'text-bad font-semibold' : ''}`}>{label}</T>
      <View className="flex-row items-center gap-1.5 shrink">
        {typeof value === 'string' ? <T className="text-muted text-[15px] text-right shrink">{value}</T> : value}
        {onPress && !danger && <T className="text-muted text-lg">›</T>}
      </View>
    </View>
  );
  return onPress ? (
    <Pressable onPress={onPress} className="active:opacity-60" accessibilityRole="button">
      {content}
    </Pressable>
  ) : (
    content
  );
}

/** For a sheet opened with a link it cannot show, such as an unknown metric. */
export function NotFound({ message = 'This page doesn’t exist. The link may be from a newer version of the app.' }: { message?: string }) {
  return (
    <Sheet className="px-4 pt-6 pb-10 gap-4">
      <SheetHeader title="Not found" action="Close" onAction={() => (router.canGoBack() ? router.back() : router.replace('/'))} />
      <T className="text-muted leading-6">{message}</T>
    </Sheet>
  );
}
