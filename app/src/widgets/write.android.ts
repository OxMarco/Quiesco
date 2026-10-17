// Android: redraw every Room widget on the home screen with the new snapshot.

import { requestWidgetUpdate } from 'react-native-android-widget';

import { roomWidget } from './RoomWidget';
import type { WidgetSnapshot } from './snapshot';

/** Must match the widget's name in app.json (react-native-android-widget). */
export const ROOM_WIDGET = 'Room';

export async function writeSnapshot(snapshot: WidgetSnapshot): Promise<void> {
  await requestWidgetUpdate({
    widgetName: ROOM_WIDGET,
    renderWidget: (info) => roomWidget(snapshot, info.width),
  });
}
