// App entry. Background work is set up before the router, because the system
// can start the app with no screen: to run the periodic sync task, or (Android)
// to draw a home-screen widget.

import { registerWidgetTaskHandler } from 'react-native-android-widget';
import { Platform } from 'react-native';

import './src/background/task';
import { widgetTaskHandler } from './src/widgets/handler';

if (Platform.OS === 'android' && widgetTaskHandler) registerWidgetTaskHandler(widgetTaskHandler);

import 'expo-router/entry';
