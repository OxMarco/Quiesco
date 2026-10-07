import '@/global.css';

import { Fredoka_500Medium, Fredoka_600SemiBold } from '@expo-google-fonts/fredoka';
import { KiwiMaru_300Light, KiwiMaru_400Regular } from '@expo-google-fonts/kiwi-maru';
import { NotoSans_400Regular, NotoSans_500Medium, NotoSans_600SemiBold } from '@expo-google-fonts/noto-sans';
import { useFonts } from 'expo-font';
import { DarkTheme, DefaultTheme, Stack, ThemeProvider } from 'expo-router';
import * as SplashScreen from 'expo-splash-screen';
import { StatusBar } from 'expo-status-bar';
import { useEffect } from 'react';
import { View } from 'react-native';
import { GestureHandlerRootView } from 'react-native-gesture-handler';

import { boot } from '@/ble/session';
import { SetupKeyPrompt } from '@/components/setup-key-prompt';
import { trace } from '@/debug/trace';
import { themeVars, useTheme } from '@/theme';

SplashScreen.preventAutoHideAsync();

export default function RootLayout() {
  const t = useTheme();
  const [fontsLoaded, fontError] = useFonts({
    KiwiMaru_300Light,
    KiwiMaru_400Regular,
    Fredoka_500Medium,
    Fredoka_600SemiBold,
    NotoSans_400Regular,
    NotoSans_500Medium,
    NotoSans_600SemiBold,
  });
  const ready = fontsLoaded || fontError !== null;

  useEffect(() => {
    if (!ready) return;
    SplashScreen.hideAsync();
    boot().catch((e) => trace('boot failed', e));
  }, [ready]);

  if (!ready) return null;

  const base = t.dark ? DarkTheme : DefaultTheme;
  const navTheme = {
    ...base,
    colors: {
      ...base.colors,
      primary: t.primary,
      background: t.bg,
      card: t.bg,
      text: t.text,
      border: t.line,
    },
  };

  return (
    <GestureHandlerRootView style={{ flex: 1 }}>
      <ThemeProvider value={navTheme}>
        <View style={t.dark ? themeVars.night : themeVars.day} className="flex-1 bg-bg">
          <StatusBar style={t.dark ? 'light' : 'dark'} />
          <Stack screenOptions={{ contentStyle: { backgroundColor: t.bg } }}>
            <Stack.Screen name="(tabs)" options={{ headerShown: false }} />
            <Stack.Screen name="connect" options={{ presentation: 'modal', headerShown: false }} />
            <Stack.Screen name="calibrate" options={{ presentation: 'modal', headerShown: false }} />
            <Stack.Screen name="sleep-window" options={{ presentation: 'modal', headerShown: false }} />
            <Stack.Screen name="why/[metric]" options={{ presentation: 'modal', headerShown: false }} />
            <Stack.Screen name="metric/[metric]" options={{ presentation: 'modal', headerShown: false }} />
          </Stack>
          <SetupKeyPrompt />
        </View>
      </ThemeProvider>
    </GestureHandlerRootView>
  );
}
