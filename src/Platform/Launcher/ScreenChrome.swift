// The title of a pushed screen, drawn by the launcher on both platforms so that it sits on the backdrop the same way: a glass back
// button (a circle, 52 pt), the title in large type, and what the screen puts at its right.
import SwiftUI

struct ScreenChrome<Trailing: View>: ViewModifier {
    @EnvironmentObject private var model: LauncherModel
    let title: String
    var subtitle: String?
    @ViewBuilder var trailing: () -> Trailing

    func body(content: Content) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 16) {
                Button { model.pop() } label: {
                    Image(systemName: "chevron.left")
                        .font(.title3.weight(.semibold))
                        .frame(width: 52, height: 52)
                        .contentShape(Circle())
                }
                .glassButton()
                .buttonBorderShape(.circle)
                .accessibilityLabel("Back")
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).launcherTitle(34).lineLimit(1)
                    if let subtitle {
                        Text(subtitle).font(.callout).foregroundStyle(Theme.secondaryText).lineLimit(1)
                    }
                }
                Spacer(minLength: 12)
                trailing()
            }
            .padding(.horizontal, 28)
            .padding(.top, 14)
            .padding(.bottom, 14)
            .frame(maxWidth: 820)
            content
        }
        .frame(maxWidth: .infinity)
    }
}

extension View {
    func launcherScreen<Trailing: View>(_ title: String, subtitle: String? = nil, @ViewBuilder trailing: @escaping () -> Trailing) -> some View {
        modifier(ScreenChrome(title: title, subtitle: subtitle, trailing: trailing))
    }

    func launcherScreen(_ title: String, subtitle: String? = nil) -> some View {
        modifier(ScreenChrome(title: title, subtitle: subtitle, trailing: { EmptyView() }))
    }
}

/// A group of rows or facts on one piece of glass, with a small caption.
struct GlassCard<Content: View>: View {
    var title: String?
    @ViewBuilder var content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let title {
                Text(title.uppercased()).font(.footnote.weight(.semibold)).tracking(1.2).foregroundStyle(Theme.secondaryText)
            }
            content()
        }
        .padding(Theme.cardPadding)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface(radius: Theme.cardRadius)
    }
}

/// The tile an icon sits in at the left of a row: its radius is the row's minus the row's padding.
struct IconTile: View {
    let systemImage: String
    var tint: Color = Theme.amber

    var body: some View {
        Image(systemName: systemImage)
            .font(.system(size: 20, weight: .semibold))
            .foregroundStyle(tint)
            .frame(width: 44, height: 44)
            .background(RoundedRectangle(cornerRadius: Theme.tileRadius, style: .continuous).fill(tint.opacity(0.18)))
    }
}

/// A row-sized button on glass: icon tile, title, a second line, a value and a chevron when it opens something.
struct MenuRow: View {
    let title: String
    var subtitle: String?
    let systemImage: String
    var value: String?
    var chevron = true
    var tint: Color = Theme.amber
    var glassTint: GlassTint = .none
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 16) {
                IconTile(systemImage: systemImage, tint: tint)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.title3.weight(.semibold)).foregroundStyle(Theme.primaryText)
                    if let subtitle {
                        Text(subtitle).font(.subheadline).foregroundStyle(Theme.secondaryText).fixedSize(horizontal: false, vertical: true)
                    }
                }
                Spacer(minLength: 8)
                if let value { Text(value).font(.callout.monospacedDigit()).foregroundStyle(Theme.secondaryText) }
                if chevron { Image(systemName: "chevron.right").font(.footnote.weight(.bold)).foregroundStyle(Theme.tertiaryText) }
            }
            .padding(Theme.rowPadding)
            .frame(maxWidth: .infinity, minHeight: Theme.minHit + 16, alignment: .leading)
        }
        .buttonStyle(GlassRowButtonStyle(tint: glassTint))
    }
}
