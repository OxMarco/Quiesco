// Android asks for the widget when it is added, resized or due for its periodic
// update (updatePeriodMillis in app.json), possibly with the app not running.

import type { WidgetTaskHandlerProps } from 'react-native-android-widget';

import { currentSnapshot } from '.';
import { roomWidget } from './RoomWidget';

export async function widgetTaskHandler({ widgetAction, widgetInfo, renderWidget }: WidgetTaskHandlerProps): Promise<void> {
  if (widgetAction === 'WIDGET_DELETED' || widgetAction === 'WIDGET_CLICK') return;
  renderWidget(roomWidget(await currentSnapshot(), widgetInfo.width));
}
