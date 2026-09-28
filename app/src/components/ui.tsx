// Shared primitives. Tailwind only sees literal class strings, so variants
// are spelled out in full maps rather than built by concatenation.

import { ActivityIndicator, Pressable, RefreshControl, ScrollView, Text, TextProps, View, ViewProps } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { Severity } from '@/protocol/comfort';
import { useTheme } from '@/theme';

export function T({ className = '', ...props }: TextProps & { className?: string }) {
  return <Text className={`font-body text-ink ${className}`} {...props} />;
}

export function Screen({
  children,
  scroll = true,
  onRefresh,
  refreshing = false,
}: {
  children: React.ReactNode;
  scroll?: boolean;
  /** Pull to refresh, when given. */
  onRefresh?: () => void;
  refreshing?: boolean;
}) {
  const t = useTheme();
  if (!scroll) {
    return (
      <SafeAreaView edges={['top']} className="flex-1 bg-bg px-4">
        {children}
      </SafeAreaView>
    );
  }
  return (
    <SafeAreaView edges={['top']} className="flex-1 bg-bg">
      <ScrollView
        contentContainerClassName="px-4 pb-10 gap-3"
        contentInsetAdjustmentBehavior="automatic"
        refreshControl={onRefresh ? <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={t.textMuted} /> : undefined}>
        {children}
      </ScrollView>
    </SafeAreaView>
  );
}

export function Title({ children, detail }: { children: React.ReactNode; detail?: React.ReactNode }) {
  return (
    <View className="flex-row items-end justify-between pt-3 pb-1 gap-3">
      <T className="font-wordmark text-[30px] leading-[38px] shrink" numberOfLines={1}>
        {children}
      </T>
      {detail}
    </View>
  );
}

export function Card({ className = '', ...props }: ViewProps & { className?: string }) {
  return <View className={`bg-surface rounded-card p-4 ${className}`} {...props} />;
}

export function SectionLabel({ children }: { children: React.ReactNode }) {
  return <T className="font-body-semi text-muted text-xs uppercase tracking-widest mt-3 px-1">{children}</T>;
}

type ButtonKind = 'primary' | 'secondary' | 'danger';

const BUTTON: Record<ButtonKind, { box: string; text: string }> = {
  primary: { box: 'bg-primary', text: 'text-on-primary' },
  secondary: { box: 'bg-raised', text: 'text-ink' },
  danger: { box: 'bg-bad/15', text: 'text-bad' },
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
      onPress={onPress}
      disabled={disabled || busy}
      className={`${style.box} h-12 rounded-full flex-row items-center justify-center gap-2 px-5 active:opacity-80 ${disabled ? 'opacity-40' : ''}`}>
      {busy && <ActivityIndicator color={kind === 'primary' ? t.onPrimary : t.text} />}
      <T className={`font-body-semi text-[15px] ${style.text}`}>{title}</T>
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
    <View className="flex-row items-center gap-1.5 bg-surface rounded-full px-2.5 py-1">
      <View className={`w-1.5 h-1.5 rounded-full ${PILL_DOT[tone]}`} />
      <T className="font-body-semi text-[11px] text-muted">{children}</T>
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
  const content = (
    <View className={`flex-row items-center justify-between py-3.5 gap-4 ${last ? '' : 'border-b border-line'}`}>
      <T className={`text-[15px] shrink ${danger ? 'text-bad font-semibold' : ''}`}>{label}</T>
      <View className="flex-row items-center gap-1.5 shrink-0">
        {typeof value === 'string' ? <T className="text-muted text-[15px]">{value}</T> : value}
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
