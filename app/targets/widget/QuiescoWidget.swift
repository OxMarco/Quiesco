// The Room widget: the shown unit's verdict and values, as the app last saw
// them. The app writes a JSON snapshot to the App Group on every new reading
// (src/widgets/write.ios.ts) and reloads the timeline; the same snapshot draws
// the Android widget (src/widgets/RoomWidget.tsx).

import SwiftUI
import WidgetKit

private let appGroup = "group.rest.quiesco.app"

struct Snapshot: Decodable {
    let unit: String
    let measuredAt: Double
    let headline: String
    let tone: String
    let co2: String
    let temperature: String
    let humidity: String
    let noise: String
    let summary: String

    static let sample = Snapshot(
        unit: "Bedroom", measuredAt: Date().timeIntervalSince1970 * 1000, headline: "All comfortable", tone: "ok",
        co2: "640", temperature: "19.5°C", humidity: "48%", noise: "31 dB(A)",
        summary: "CO₂ 640 ppm · 19.5°C · 48% · 31 dB(A)")

    static func load() -> Snapshot? {
        guard let json = UserDefaults(suiteName: appGroup)?.string(forKey: "snapshot"),
              let data = json.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(Snapshot.self, from: data)
    }

    var measured: Date? { measuredAt > 0 ? Date(timeIntervalSince1970: measuredAt / 1000) : nil }

    var toneColor: Color {
        switch tone {
        case "bad": return Color("bad")
        case "warn": return Color("warn")
        case "ok": return Color("tide")
        default: return Color("muted")
        }
    }
}

struct RoomEntry: TimelineEntry {
    let date: Date
    let snapshot: Snapshot?
}

struct RoomProvider: TimelineProvider {
    func placeholder(in context: Context) -> RoomEntry { RoomEntry(date: .now, snapshot: .sample) }

    func getSnapshot(in context: Context, completion: @escaping (RoomEntry) -> Void) {
        completion(RoomEntry(date: .now, snapshot: Snapshot.load() ?? .sample))
    }

    // The app reloads on every reading; the hourly entry only keeps "updated" honest.
    func getTimeline(in context: Context, completion: @escaping (Timeline<RoomEntry>) -> Void) {
        let entry = RoomEntry(date: .now, snapshot: Snapshot.load())
        completion(Timeline(entries: [entry], policy: .after(.now.addingTimeInterval(3600))))
    }
}

private struct Updated: View {
    let s: Snapshot
    var body: some View {
        if let measured = s.measured {
            Text(measured, style: .relative).font(.caption2).foregroundStyle(Color("muted"))
        }
    }
}

private struct Co2: View {
    let s: Snapshot
    let size: CGFloat
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 3) {
            Text(s.co2).font(.system(size: size, weight: .semibold, design: .rounded)).foregroundStyle(Color("ink"))
                .minimumScaleFactor(0.6).lineLimit(1)
            Text("ppm").font(.caption).foregroundStyle(Color("muted"))
        }
    }
}

private struct Value: View {
    let value: String
    let label: String
    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(value).font(.system(.title3, design: .rounded, weight: .semibold)).foregroundStyle(Color("ink"))
                .minimumScaleFactor(0.6).lineLimit(1)
            Text(label).font(.caption2).foregroundStyle(Color("muted")).lineLimit(1)
        }
    }
}

struct RoomWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let entry: RoomEntry

    var body: some View {
        let s = entry.snapshot ?? Snapshot(
            unit: "Quiesco", measuredAt: 0, headline: "Open the app to add your Quiesco", tone: "none",
            co2: "--", temperature: "--", humidity: "--", noise: "--", summary: "")
        switch family {
        case .accessoryInline:
            Text("\(s.unit): \(s.headline)")
        case .accessoryRectangular:
            VStack(alignment: .leading, spacing: 1) {
                Text(s.headline).font(.headline).lineLimit(1)
                if s.measured != nil {
                    Text("CO₂ \(s.co2) ppm · \(s.temperature)").font(.caption).lineLimit(1)
                }
            }
        case .systemMedium:
            HStack(alignment: .top, spacing: 16) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(s.unit).font(.caption).foregroundStyle(Color("muted")).lineLimit(1)
                    Text(s.headline).font(.subheadline.weight(.semibold)).foregroundStyle(s.toneColor).lineLimit(2)
                    Spacer(minLength: 0)
                    if s.measured != nil { Co2(s: s, size: 34) }
                    Updated(s: s)
                }
                if s.measured != nil {
                    VStack(alignment: .leading, spacing: 8) {
                        Value(value: s.temperature, label: "Temperature")
                        Value(value: s.humidity, label: "Humidity")
                        Value(value: s.noise, label: "Noise")
                    }
                }
            }
        default:
            VStack(alignment: .leading, spacing: 4) {
                Text(s.unit).font(.caption).foregroundStyle(Color("muted")).lineLimit(1)
                Text(s.headline).font(.subheadline.weight(.semibold)).foregroundStyle(s.toneColor).lineLimit(2)
                Spacer(minLength: 0)
                if s.measured != nil { Co2(s: s, size: 30) }
                Updated(s: s)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

struct QuiescoWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: "QuiescoRoom", provider: RoomProvider()) { entry in
            RoomWidgetView(entry: entry)
                .containerBackground(for: .widget) { Color("$widgetBackground") }
                .widgetURL(URL(string: "quiesco://"))
                .accessibilityElement(children: .combine)
        }
        .configurationDisplayName("Room")
        .description("How the room is right now, with the problem to fix first")
        .supportedFamilies([.systemSmall, .systemMedium, .accessoryRectangular, .accessoryInline])
    }
}

@main
struct QuiescoWidgetBundle: WidgetBundle {
    var body: some Widget {
        QuiescoWidget()
    }
}
