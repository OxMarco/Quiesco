import { useState } from 'react';
import { KeyboardAvoidingView, Modal, Platform, ScrollView, TextInput, View } from 'react-native';

import { cancelEnrollment, enrollment, submitSetupKey } from '@/ble/enrollment';
import { useStore } from '@/ble/store';
import { Button, T } from '@/components/ui';
import { themeVars, useTheme } from '@/theme';

export function SetupKeyPrompt() {
  const keyId = useStore(enrollment, (s) => s.keyId);
  const t = useTheme();
  return (
    <Modal visible={keyId !== null} transparent animationType="fade" onRequestClose={() => cancelEnrollment()}>
      {/* A modal is its own native window: it needs its own theme variables,
          or the NativeWind classes inside fall back to the day palette. */}
      <View style={[{ flex: 1 }, t.dark ? themeVars.night : themeVars.day]}>
        {keyId !== null && <SetupKeyForm key={keyId} />}
      </View>
    </Modal>
  );
}

function SetupKeyForm() {
  const t = useTheme();
  const [text, setText] = useState('');
  const [error, setError] = useState<string | null>(null);
  const submit = (value = text) => {
    try { submitSetupKey(value); } catch (e) { setError(e instanceof Error ? e.message : 'Check the code and try again.'); }
  };
  const onChange = (v: string) => {
    const digits = v.replace(/\D/g, '').slice(0, 6);
    setText(digits);
    setError(null);
    if (digits.length === 6) submit(digits);
  };
  return (
    <KeyboardAvoidingView behavior={Platform.OS === 'ios' ? 'padding' : 'height'} style={{ flex: 1, backgroundColor: '#0008' }}>
      <ScrollView contentContainerStyle={{ flexGrow: 1, justifyContent: 'center', padding: 24 }} keyboardShouldPersistTaps="handled">
        <View style={{ backgroundColor: t.bg, borderRadius: 24, padding: 24, gap: 16 }}>
          <T className="font-body-semi text-xl">Enter the code on your Quiesco</T>
          <T className="leading-6">Keep the unit plugged into USB. Its screen shows a six-digit code.</T>
          <TextInput
            accessibilityLabel="Six-digit code from the unit’s screen"
            value={text} onChangeText={onChange} keyboardType="number-pad" autoCorrect={false}
            autoComplete="off" textContentType="none" maxLength={6} autoFocus
            placeholder="000000" placeholderTextColor={t.textMuted}
            style={{ color: t.text, borderColor: t.line, borderWidth: 1, borderRadius: 12, padding: 14, fontSize: 28, letterSpacing: 8, textAlign: 'center' }}
          />
          {error && <T className="text-bad" accessibilityRole="alert">{error}</T>}
          <Button title="Add this phone" disabled={text.length !== 6} onPress={() => submit()} />
          <Button title="Cancel" kind="secondary" onPress={() => cancelEnrollment()} />
        </View>
      </ScrollView>
    </KeyboardAvoidingView>
  );
}
