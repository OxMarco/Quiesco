// "Send a diagnostic report": a row that opens the report in a sheet, so the
// user sees exactly what would be sent, then emails it to support or shares
// it some other way. Nothing leaves the phone unless the user sends it.

import * as MailComposer from 'expo-mail-composer';
import { useState } from 'react';
import { Linking, Modal, Platform, Pressable, ScrollView, Share, View } from 'react-native';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';

import { link } from '@/ble/link';
import { session } from '@/ble/session';
import { buildReport, SUPPORT_EMAIL } from '@/debug/report';
import { themeVars, useTheme } from '@/theme';

import { Button, Row, T } from './ui';

const SUBJECT = 'Quiesco diagnostic report';
// Mail apps truncate or refuse very long mailto: links; past this the
// fallback keeps the head of the report and the newest log lines.
const MAILTO_MAX = 6000;

export function DiagnosticReportRow({ last }: { last?: boolean }) {
  const t = useTheme();
  const [report, setReport] = useState<string | null>(null);
  const open = () => setReport(buildReport(link.get(), session.get().units[0] ?? null));
  const close = () => setReport(null);
  return (
    <>
      <Row label="Send a diagnostic report" onPress={open} last={last} />
      <Modal visible={report !== null} animationType="slide" presentationStyle="pageSheet" onRequestClose={close}>
        {/* A modal is its own native window: it needs its own safe-area
            provider and theme variables. */}
        <SafeAreaProvider>
          <View style={[{ flex: 1 }, t.dark ? themeVars.night : themeVars.day]}>
            {report !== null && <ReportSheet report={report} onClose={close} />}
          </View>
        </SafeAreaProvider>
      </Modal>
    </>
  );
}

function ReportSheet({ report, onClose }: { report: string; onClose: () => void }) {
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<{ text: string; bad: boolean } | null>(null);

  const email = async () => {
    setBusy(true);
    setNote(null);
    try {
      if (await MailComposer.isAvailableAsync()) {
        const { status } = await MailComposer.composeAsync({ recipients: [SUPPORT_EMAIL], subject: SUBJECT, body: report });
        if (status === MailComposer.MailComposerStatus.SENT) setNote({ text: 'Sent. Thank you, we’ll be in touch.', bad: false });
      } else {
        const url = `mailto:${SUPPORT_EMAIL}?subject=${encodeURIComponent(SUBJECT)}&body=${encodeURIComponent(forMailto(report))}`;
        await Linking.openURL(url);
      }
    } catch {
      setNote({ text: `Couldn’t open a mail app. Use Share and send it to ${SUPPORT_EMAIL}.`, bad: true });
    } finally {
      setBusy(false);
    }
  };

  const share = () => {
    setNote(null);
    Share.share({ title: SUBJECT, message: report }).catch(() => setNote({ text: 'Couldn’t open the share sheet.', bad: true }));
  };

  return (
    <SafeAreaView edges={['top', 'bottom']} className="flex-1 bg-bg">
      <View className="flex-row items-center justify-between px-5 pt-4 pb-2">
        <T className="font-body-semi text-lg">Diagnostic report</T>
        <Pressable onPress={onClose} accessibilityRole="button" hitSlop={12}>
          <T className="font-body-semi text-[15px] text-primary">Close</T>
        </Pressable>
      </View>
      <T className="px-5 pb-3 text-[13px] text-muted leading-[18px]">
        This is everything the report contains: the app and phone versions, the unit’s status and the app’s recent
        Bluetooth log. No readings and no keys. It is only sent if you send it.
      </T>
      <ScrollView className="flex-1 mx-4 rounded-card bg-surface" contentContainerClassName="p-3">
        <T selectable className="text-[11px] leading-[15px]" style={{ fontFamily: Platform.select({ ios: 'Menlo', default: 'monospace' }) }}>
          {report}
        </T>
      </ScrollView>
      <View className="px-4 pt-3 pb-2 gap-2">
        {note && <T className={`text-[13px] text-center leading-[18px] ${note.bad ? 'text-bad' : 'text-muted'}`}>{note.text}</T>}
        <Button title={`Email to ${SUPPORT_EMAIL}`} onPress={email} busy={busy} />
        <Button title="Share…" kind="secondary" onPress={share} />
      </View>
    </SafeAreaView>
  );
}

/** The report cut to fit a mailto: link: its head, then the newest lines. */
function forMailto(report: string): string {
  if (report.length <= MAILTO_MAX) return report;
  const head = report.slice(0, report.indexOf('## Log'));
  const tail = report.slice(-(MAILTO_MAX - head.length - 40));
  return `${head}## Log (cut to fit; use Share for all of it)\n…${tail.slice(tail.indexOf('\n'))}`;
}
