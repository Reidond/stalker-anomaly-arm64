import Foundation
import ImageIO
import AppKit
import SwiftUI
@testable import ModManagerCore

// FOMOD: parsing (encodings, case), every group type, dependency types, flags, conditional installs, priority, the wizard.

/// A small picture with a sky, a horizon and a few shapes: enough to see how images sit in the wizard.
private let onePixelPNG: Data = {
    let w = 640, h = 360
    let space = CGColorSpaceCreateDeviceRGB()
    guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return Data() }
    let sky = [CGColor(red: 0.10, green: 0.12, blue: 0.10, alpha: 1), CGColor(red: 0.62, green: 0.38, blue: 0.16, alpha: 1)] as CFArray
    if let gradient = CGGradient(colorsSpace: space, colors: sky, locations: [0, 1]) {
        ctx.drawLinearGradient(gradient, start: CGPoint(x: 0, y: CGFloat(h)), end: CGPoint(x: 0, y: CGFloat(h) * 0.35), options: [])
    }
    ctx.setFillColor(CGColor(red: 0.05, green: 0.06, blue: 0.05, alpha: 1))
    ctx.fill(CGRect(x: 0, y: 0, width: w, height: Int(Double(h) * 0.38)))
    ctx.setFillColor(CGColor(red: 0.74, green: 0.92, blue: 0.22, alpha: 0.9))
    ctx.fillEllipse(in: CGRect(x: 470, y: 150, width: 44, height: 44))
    ctx.setFillColor(CGColor(red: 0.03, green: 0.04, blue: 0.03, alpha: 1))
    for (x, height) in [(60, 150), (130, 110), (190, 180), (300, 90), (380, 130)] {
        ctx.fill(CGRect(x: x, y: 0, width: 34, height: height))
    }
    guard let image = ctx.makeImage() else { return Data() }
    let data = NSMutableData()
    guard let dest = CGImageDestinationCreateWithData(data, "public.png" as CFString, 1, nil) else { return Data() }
    CGImageDestinationAddImage(dest, image, nil)
    CGImageDestinationFinalize(dest)
    return data as Data
}()

/// A config that uses every kind of group, a dependent type, a hidden step, flags, a conditional install and priorities.
/// Source paths are written in different case and with backslashes on purpose.
let overhaulConfig = """
<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
  <moduleName>Test Overhaul &amp; More</moduleName>
  <moduleImage path="fomod\\images\\banner.png" />
  <requiredInstallFiles>
    <folder source="core\\GameData" destination="gamedata" />
  </requiredInstallFiles>
  <installSteps order="Explicit">
    <installStep name="Weapons">
      <optionalFileGroups order="Explicit">
        <group name="Pack" type="SelectExactlyOne">
          <plugins order="Explicit">
            <plugin name="Vanilla+">
              <description>Keeps the stock balance. Fish &amp; chips.</description>
              <image path="fomod/images/vanilla.png" />
              <conditionFlags><flag name="pack">vanilla</flag></conditionFlags>
              <files><file source="options/VANILLA/w.ltx" destination="gamedata/configs/w.ltx" priority="1" /></files>
              <typeDescriptor><type name="Recommended" /></typeDescriptor>
            </plugin>
            <plugin name="Hardcore">
              <description>Everything hurts more.</description>
              <image path="fomod/images/missing.png" />
              <conditionFlags><flag name="pack">hardcore</flag></conditionFlags>
              <files>
                <folder source="Options/Hardcore" destination="gamedata" />
              </files>
              <typeDescriptor><type name="Optional" /></typeDescriptor>
            </plugin>
          </plugins>
        </group>
        <group name="Always" type="SelectAll">
          <plugins order="Explicit">
            <plugin name="Base tweaks"><description>d</description><files><file source="options/base.ltx" destination="gamedata/configs/base.ltx" /></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
            <plugin name="Base sounds"><description>d</description><files><file source="options/sound.ogg" destination="gamedata/sounds/s.ogg" /></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="Attachments">
      <optionalFileGroups order="Explicit">
        <group name="Extras" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="Silencers">
              <description>Quiet.</description>
              <conditionFlags><flag name="extras">yes</flag></conditionFlags>
              <files><file source="options/silencers.ltx" destination="gamedata/configs/silencers.ltx" /></files>
              <typeDescriptor><type name="Optional"/></typeDescriptor>
            </plugin>
            <plugin name="Scopes">
              <description>Needs the vanilla pack.</description>
              <files><file source="options/scopes.ltx" /></files>
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns>
                    <pattern>
                      <dependencies operator="And"><flagDependency flag="pack" value="hardcore"/></dependencies>
                      <type name="NotUsable"/>
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
            </plugin>
            <plugin name="Locked on">
              <description>Always there.</description>
              <files><file source="options/locked.ltx" destination="gamedata/configs/locked.ltx" /></files>
              <typeDescriptor><type name="Required"/></typeDescriptor>
            </plugin>
          </plugins>
        </group>
        <group name="Sights" type="SelectAtMostOne">
          <plugins order="Explicit">
            <plugin name="Red dot"><description>d</description><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
            <plugin name="Holo"><description>d</description><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
          </plugins>
        </group>
        <group name="Pick some" type="SelectAtLeastOne">
          <plugins order="Explicit">
            <plugin name="Alpha"><description>d</description><files><file source="options/alpha.ltx" destination="gamedata/configs/alpha.ltx" /></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
            <plugin name="Beta"><description>d</description><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="Hardcore tweaks">
      <visible><flagDependency flag="pack" value="hardcore"/></visible>
      <optionalFileGroups>
        <group name="Tweaks" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="Bleeding"><description>d</description><files><file source="options/bleed.ltx" destination="gamedata/configs/bleed.ltx" /></files><typeDescriptor><type name="Required"/></typeDescriptor></plugin>
            <plugin name="Fatigue"><description>d</description><files><file source="options/fatigue.ltx" destination="gamedata/configs/fatigue.ltx" /></files><typeDescriptor><type name="CouldBeUsable"/></typeDescriptor></plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="Patch">
      <visible>
        <dependencies operator="Or">
          <flagDependency flag="extras" value="yes"/>
          <fileDependency file="gamedata/scripts/other.script" state="Active"/>
        </dependencies>
      </visible>
      <optionalFileGroups>
        <group name="Patch group" type="SelectExactlyOne">
          <plugins order="Explicit">
            <plugin name="Patch A"><description>d</description><files><file source="options/patch.ltx" destination="gamedata/configs/patch.ltx" /></files><typeDescriptor><type name="Recommended"/></typeDescriptor></plugin>
            <plugin name="Patch B"><description>d</description><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
  <conditionalFileInstalls>
    <patterns>
      <pattern>
        <dependencies operator="And">
          <flagDependency flag="pack" value="hardcore"/>
          <flagDependency flag="extras" value="yes"/>
        </dependencies>
        <files><file source="options/combo.ltx" destination="gamedata/configs/combo.ltx" /></files>
      </pattern>
    </patterns>
  </conditionalFileInstalls>
</config>
"""

let overhaulInfo = """
<fomod><Name>Test Overhaul</Name><Author>Someone</Author><Version MachineVersion="1.0">1.4.0</Version><Description>Does things.</Description><Website>https://example.invalid</Website></fomod>
"""

private func overhaulEntries(configData: Data, infoData: Data? = nil) -> [FixtureEntry] {
    [
        FixtureEntry("Test Overhaul/fomod/ModuleConfig.xml", data: configData),
        FixtureEntry("Test Overhaul/fomod/info.xml", data: infoData ?? Data(overhaulInfo.utf8)),
        FixtureEntry("Test Overhaul/fomod/images/vanilla.png", data: onePixelPNG),
        FixtureEntry("Test Overhaul/fomod/images/banner.png", data: onePixelPNG),
        FixtureEntry("Test Overhaul/Core/gamedata/scripts/core.script", "core"),
        FixtureEntry("Test Overhaul/Core/gamedata/configs/w.ltx", "core-w"),
        FixtureEntry("Test Overhaul/Options/vanilla/w.ltx", "vanilla-w"),
        FixtureEntry("Test Overhaul/Options/Hardcore/configs/w.ltx", "hardcore-w"),
        FixtureEntry("Test Overhaul/Options/Hardcore/scripts/hc.script", "hc"),
        FixtureEntry("Test Overhaul/Options/base.ltx", "base"),
        FixtureEntry("Test Overhaul/Options/sound.ogg", "ogg"),
        FixtureEntry("Test Overhaul/Options/silencers.ltx", "sil"),
        FixtureEntry("Test Overhaul/Options/scopes.ltx", "scopes"),
        FixtureEntry("Test Overhaul/Options/locked.ltx", "locked"),
        FixtureEntry("Test Overhaul/Options/alpha.ltx", "alpha"),
        FixtureEntry("Test Overhaul/Options/bleed.ltx", "bleed"),
        FixtureEntry("Test Overhaul/Options/fatigue.ltx", "fatigue"),
        FixtureEntry("Test Overhaul/Options/patch.ltx", "patch"),
        FixtureEntry("Test Overhaul/Options/combo.ltx", "combo"),
    ]
}

private func utf16(_ text: String, bigEndian: Bool = false, declaration: String? = nil) -> Data {
    var body = text
    if let declaration { body = body.replacingOccurrences(of: #"^<\?xml[^>]*\?>"#, with: declaration, options: .regularExpression) }
    var data = Data(bigEndian ? [0xFE, 0xFF] : [0xFF, 0xFE])
    data.append(body.data(using: bigEndian ? .utf16BigEndian : .utf16LittleEndian)!)
    return data
}

private func installerFrom(_ config: Data, info: Data? = nil) throws -> FomodInstaller {
    try FomodParser.parse(config: config, info: info, rootPrefix: "", assets: FomodAssets())
}

private func ref(_ step: Int, _ group: Int, _ plugin: Int) -> FomodPluginRef { FomodPluginRef(step: step, group: group, plugin: plugin) }

func registerFomodTests() {
    test("fomod parse: structure, orders, types, flags and files") {
        let installer = try installerFrom(Data(overhaulConfig.utf8), info: Data(overhaulInfo.utf8))
        try expectEqual(installer.moduleName, "Test Overhaul & More")
        try expectEqual(installer.info.version, "1.4.0")
        try expectEqual(installer.info.author, "Someone")
        try expectEqual(installer.steps.map(\.name), ["Weapons", "Attachments", "Hardcore tweaks", "Patch"])
        try expectEqual(installer.steps[0].groups.map(\.type), [.selectExactlyOne, .selectAll])
        try expectEqual(installer.steps[1].groups.map(\.type), [.selectAny, .selectAtMostOne, .selectAtLeastOne])
        try expectEqual(installer.steps[0].groups[0].plugins[0].description, "Keeps the stock balance. Fish & chips.")
        try expect(installer.steps[2].visible != nil)
        try expectEqual(installer.requiredFiles.count, 1)
        try expect(installer.requiredFiles[0].isFolder)
        try expectEqual(installer.requiredFiles[0].source, "core/GameData")
        try expectEqual(installer.conditionalInstalls.count, 1)
        let vanillaFile = installer.steps[0].groups[0].plugins[0].files[0]
        try expectEqual(vanillaFile.priority, 1)
        try expectEqual(vanillaFile.destination, "gamedata/configs/w.ltx")
        // an omitted destination is nil, an empty one is ""
        try expectEqual(installer.steps[1].groups[0].plugins[1].files[0].destination, nil)
    }

    test("fomod parse: order attribute sorts steps, groups and plugins") {
        let xml = """
        <config><moduleName>X</moduleName><installSteps order="Descending">
          <installStep name="b"><optionalFileGroups order="Ascending">
            <group name="z" type="SelectAny"><plugins order="Descending"><plugin name="a"/><plugin name="c"/><plugin name="B"/></plugins></group>
            <group name="y" type="SelectAny"><plugins><plugin name="q"/></plugins></group>
          </optionalFileGroups></installStep>
          <installStep name="a"><optionalFileGroups/></installStep>
          <installStep name="c"><optionalFileGroups/></installStep>
        </installSteps></config>
        """
        let installer = try installerFrom(Data(xml.utf8))
        try expectEqual(installer.steps.map(\.name), ["c", "b", "a"])
        try expectEqual(installer.steps[1].groups.map(\.name), ["y", "z"])
        try expectEqual(installer.steps[1].groups[1].plugins.map(\.name), ["c", "B", "a"])
    }

    test("fomod parse: UTF-16 (BOM, no BOM, wrong declaration), element case and bare ampersands") {
        let reference = try installerFrom(Data(overhaulConfig.utf8))
        let variants: [(String, Data)] = [
            ("utf16le bom", utf16(overhaulConfig, declaration: #"<?xml version="1.0" encoding="UTF-16"?>"#)),
            ("utf16be bom", utf16(overhaulConfig, bigEndian: true)),
            ("utf16 bom, declaration says utf-8", utf16(overhaulConfig)),
            ("utf8 bom", Data([0xEF, 0xBB, 0xBF]) + Data(overhaulConfig.utf8)),
            ("utf16le without bom", overhaulConfig.data(using: .utf16LittleEndian)!),
            ("shouting tags", Data(overhaulConfig.replacingOccurrences(of: "installStep", with: "INSTALLSTEP").replacingOccurrences(of: "moduleName", with: "ModuleName").utf8)),
            ("bare ampersand", Data(overhaulConfig.replacingOccurrences(of: "Fish &amp; chips", with: "Fish & chips").utf8)),
        ]
        for (name, data) in variants {
            let installer = try installerFrom(data)
            try expectEqual(installer.steps.map(\.name), reference.steps.map(\.name), name)
            try expectEqual(installer.moduleName, "Test Overhaul & More", name)
            try expectEqual(installer.steps[0].groups[0].plugins[0].description, "Keeps the stock balance. Fish & chips.", name)
        }
        try await expectError(matches: { if case ModImportError.invalidFomod = $0 { return true } else { return false } }) {
            _ = try installerFrom(Data("<config><unclosed></config>".utf8))
        }
        try await expectError(matches: { if case ModImportError.invalidFomod = $0 { return true } else { return false } }) {
            _ = try installerFrom(Data("<other/>".utf8))
        }
    }

    test("fomod session: group rules, dependent types, hidden steps") {
        let installer = try installerFrom(Data(overhaulConfig.utf8))
        let s = FomodSession(installer: installer)
        // step 1 defaults: Vanilla+ (recommended) is on, Hardcore off, the SelectAll group is on and locked
        try expectEqual(s.currentStep, 0)
        try expect(s.isSelected(ref(0, 0, 0)) && !s.isSelected(ref(0, 0, 1)))
        try expect(s.isSelected(ref(0, 1, 0)) && s.isSelected(ref(0, 1, 1)))
        try expect(s.isLocked(ref(0, 1, 0)))
        s.toggle(ref(0, 1, 0))
        try expect(s.isSelected(ref(0, 1, 0)), "SelectAll cannot be unselected")
        // SelectExactlyOne: picking another turns the first off, picking the same one again does nothing
        s.toggle(ref(0, 0, 1))
        try expect(!s.isSelected(ref(0, 0, 0)) && s.isSelected(ref(0, 0, 1)))
        s.toggle(ref(0, 0, 1))
        try expect(s.isSelected(ref(0, 0, 1)), "ExactlyOne keeps its choice")
        try expect(s.canGoNext && !s.canGoBack)
        s.next()

        // step 2: Scopes is unusable now that pack=hardcore; Required is on and locked; AtLeastOne preselected Alpha
        try expectEqual(s.currentStep, 1)
        try expectEqual(s.effectiveType(ref(1, 0, 1)), .notUsable)
        try expect(s.isLocked(ref(1, 0, 1)))
        s.toggle(ref(1, 0, 1))
        try expect(!s.isSelected(ref(1, 0, 1)), "NotUsable cannot be selected")
        try expect(s.isSelected(ref(1, 0, 2)) && s.isLocked(ref(1, 0, 2)))
        try expect(!s.isSelected(ref(1, 1, 0)) && !s.isSelected(ref(1, 1, 1)))
        s.toggle(ref(1, 1, 0))
        s.toggle(ref(1, 1, 1))
        try expect(!s.isSelected(ref(1, 1, 0)) && s.isSelected(ref(1, 1, 1)), "AtMostOne is a radio")
        s.toggle(ref(1, 1, 1))
        try expect(!s.isSelected(ref(1, 1, 1)), "AtMostOne can be cleared")
        try expect(s.isSelected(ref(1, 2, 0)), "AtLeastOne starts with its first usable option")
        s.toggle(ref(1, 2, 0))
        try expect(s.problem(in: FomodGroupRef(step: 1, group: 2)) != nil)
        try expect(!s.canGoNext, "AtLeastOne with nothing selected blocks Next")
        s.toggle(ref(1, 2, 1))
        try expect(s.canGoNext)

        // the hardcore step is visible, the patch step is not (no extras flag yet)
        s.next()
        try expectEqual(s.currentStep, 2)
        try expect(s.isSelected(ref(2, 0, 0)) && s.isLocked(ref(2, 0, 0)), "Required stays on")
        try expectEqual(s.effectiveType(ref(2, 0, 1)), .couldBeUsable)
        try expect(s.isLastStep && s.canFinish)

        // back, turn Silencers on: the flag opens the Patch step
        s.back()
        s.back()
        try expectEqual(s.currentStep, 0)
        s.next()
        s.toggle(ref(1, 0, 0))
        s.next()
        s.next()
        try expectEqual(s.currentStep, 3)
        try expect(s.isSelected(ref(3, 0, 0)), "Recommended patch preselected")
        try expect(s.isLastStep)
        let choices = s.choices()
        try expectEqual(choices.selections.map(\.step), ["Weapons", "Weapons", "Attachments", "Attachments", "Attachments", "Hardcore tweaks", "Patch"])
        try expectEqual(choices.selections.first { $0.group == "Pack" }?.plugins, ["Hardcore"])
        try expectEqual(choices.selections.first { $0.group == "Extras" }?.plugins, ["Silencers", "Locked on"])
    }

    test("fomod session: defaults, replayed choices and a file condition") {
        let installer = try installerFrom(Data(overhaulConfig.utf8))
        // defaults: vanilla pack, no extras -> steps Weapons and Attachments only
        let defaults = installer.defaultChoices()
        try expectEqual(defaults.selections.map(\.step), ["Weapons", "Weapons", "Attachments", "Attachments", "Attachments"])
        try expectEqual(defaults.selections[0].plugins, ["Vanilla+"])
        // a file that is "active" in another mod opens the Patch step without the flag
        let environment = FomodEnvironment(fileState: { $0.lowercased() == "gamedata/scripts/other.script" ? .active : .missing })
        let withFile = installer.defaultChoices(environment: environment)
        try expectEqual(withFile.selections.last?.step, "Patch")
        // replaying saved choices reproduces the walk
        let session = FomodSession(installer: installer, choices: FomodChoices(selections: [
            .init(step: "Weapons", group: "Pack", plugins: ["Hardcore"]),
            .init(step: "Attachments", group: "Extras", plugins: ["Silencers"]),
        ]))
        while session.advanceIgnoringRules() {}
        try expectEqual(session.choices().selections.map(\.step).last, "Patch")
        try expectEqual(session.choices().selections.first { $0.group == "Pack" }?.plugins, ["Hardcore"])
    }

    test("fomod session: pad and keyboard commands move a cursor, pick options and press the footer") {
        let installer = try installerFrom(Data(overhaulConfig.utf8))
        let s = FomodSession(installer: installer)
        var finished: FomodChoices?
        var cancelled = 0
        s.hasCancelButton = true                            // a bare window with a Cancel button of its own
        s.onFinish = { finished = $0 }
        s.onCancel = { cancelled += 1 }

        try expectEqual(s.cursor, .option(ref(0, 0, 0)), "the first option starts highlighted")
        try expect(!s.showsCursor, "but it is not drawn until a pad or the keyboard is used")
        s.send(.up)
        try expect(s.showsCursor, "a command shows the cursor")
        try expectEqual(s.cursor, .option(ref(0, 0, 0)), "up at the top stays")
        s.send(.down)
        try expectEqual(s.cursor, .option(ref(0, 0, 1)))
        s.send(.confirm)                                    // pick Hardcore
        try expect(s.isSelected(ref(0, 0, 1)) && !s.isSelected(ref(0, 0, 0)))
        s.send(.down); s.send(.down); s.send(.down)         // Base tweaks, Base sounds, then off the end
        try expectEqual(s.cursor, .primary, "down from the last option reaches the footer")
        s.send(.down)
        try expectEqual(s.cursor, .primary)
        s.send(.left)
        try expectEqual(s.cursor, .cancel, "Back is not there on the first step")
        s.send(.right)
        try expectEqual(s.cursor, .primary)
        s.send(.up)
        try expectEqual(s.cursor, .option(ref(0, 1, 1)), "up from the footer goes to the last option")

        s.send(.next)                                       // R1 / Options: the primary button
        try expectEqual(s.currentStep, 1)
        try expectEqual(s.cursor, .option(ref(1, 0, 0)), "a new page starts at its first option")
        s.send(.confirm)                                    // Silencers
        try expect(s.isSelected(ref(1, 0, 0)))
        s.send(.back)                                       // Circle: previous step
        try expectEqual(s.currentStep, 0)
        try expectEqual(cancelled, 0)
        s.send(.back)                                       // on the first step it cancels
        try expectEqual(cancelled, 1)

        // footer buttons by cursor
        s.send(.next)
        try expectEqual(s.currentStep, 1)
        s.cursor = .primary
        s.send(.left)
        try expectEqual(s.cursor, .back)
        s.send(.confirm)
        try expectEqual(s.currentStep, 0)
        s.cursor = .cancel
        s.send(.confirm)
        try expectEqual(cancelled, 2)

        // Install is only pressed when the rules hold: walk to the last step and finish
        s.send(.next)                                       // Attachments
        s.send(.next)                                       // Hardcore tweaks (pack = hardcore)
        s.send(.next)                                       // Patch (Silencers = extras)
        try expectEqual(s.currentStep, 3)
        try expect(s.isLastStep)
        s.cursor = .primary
        s.send(.confirm)
        try expectEqual(finished?.selections.last?.step, "Patch")
        try expectEqual(finished?.selections.first { $0.group == "Pack" }?.plugins, ["Hardcore"])

        // a locked option under the cursor does nothing; an invalid page cannot be left
        let t = FomodSession(installer: installer)
        t.cursor = .primary
        t.send(.left)
        try expectEqual(t.cursor, .primary, "without a Cancel button or a Back step there is nothing to the left")
        t.send(.next)
        t.cursor = .option(ref(1, 2, 0))                    // Alpha, the only one in "Pick some"
        t.send(.confirm)
        try expect(!t.isSelected(ref(1, 2, 0)))
        try expect(!t.primaryEnabled)
        t.send(.next)
        try expectEqual(t.currentStep, 1, "Next is ignored while a group rule fails")
    }

    test("fomod install: defaults, then the hardcore path with conditions, priorities and case-insensitive sources") {
        try await withTempDir { dir in
            let archive = dir.appendingPathComponent("Test Overhaul 1.4.zip")
            try writeArchive(overhaulEntries(configData: Data(overhaulConfig.utf8)), to: archive)
            let library = try ModLibrary(root: dir.appendingPathComponent("Mods"))
            let importer = ModImporter(library: library)
            let plan = try await importer.inspect(archive)
            guard let installer = plan.fomod else { throw TestFailure(message: "no installer") }
            try expectEqual(plan.displayName, "Test Overhaul")
            try expectEqual(plan.version, "1.4.0")
            try expectEqual(plan.author, "Someone")
            try expect(!installer.isSynthetic)
            try expect(installer.imageURL(for: "fomod/images/vanilla.png") != nil, "image copied out of the archive")
            try expect(installer.imageURL(for: "fomod\\images\\banner.png") != nil, "backslashes and case are tolerated")
            try expect(installer.imageURL(for: "fomod/images/missing.png") == nil)

            // defaults: Core (required), Vanilla+ (priority 1 beats Core's w.ltx), SelectAll files, Locked on, Alpha
            let one = try await importer.install(plan, choices: nil) { _ in }
            let probeOne = InstalledModProbe(content: one.contentURL)
            try expectEqual(probeOne.allFiles(), [
                "gamedata/configs/alpha.ltx", "gamedata/configs/base.ltx", "gamedata/configs/locked.ltx", "gamedata/configs/w.ltx",
                "gamedata/scripts/core.script", "gamedata/sounds/s.ogg",
            ])
            try expectEqual(contentText(probeOne, "gamedata/configs/w.ltx"), "vanilla-w")
            try expectEqual(one.name, "Test Overhaul")
            try expectEqual(one.version, "1.4.0")

            // hardcore + silencers: hardcore folder, bleeding (required), combo (conditional), patch step, no scopes
            let session = FomodSession(installer: installer)
            session.toggle(ref(0, 0, 1))
            session.next()
            session.toggle(ref(1, 0, 0))
            session.toggle(ref(1, 0, 1))   // unusable: ignored
            session.next()
            session.toggle(ref(2, 0, 1))   // Fatigue
            session.next()
            try expect(session.canFinish)
            let chosen = session.choices()
            let two = try await importer.install(plan, choices: chosen) { _ in }
            let probeTwo = InstalledModProbe(content: two.contentURL)
            try expectEqual(probeTwo.allFiles(), [
                "gamedata/configs/alpha.ltx", "gamedata/configs/base.ltx", "gamedata/configs/bleed.ltx", "gamedata/configs/combo.ltx",
                "gamedata/configs/fatigue.ltx", "gamedata/configs/locked.ltx", "gamedata/configs/patch.ltx",
                "gamedata/configs/silencers.ltx", "gamedata/configs/w.ltx", "gamedata/scripts/core.script", "gamedata/scripts/hc.script",
                "gamedata/sounds/s.ogg",
            ])
            // the hardcore folder is installed into gamedata/, its w.ltx replaces Core's (same priority, later in the list)
            try expectEqual(contentText(probeTwo, "gamedata/configs/w.ltx"), "hardcore-w")

            let manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: two.folderURL.appendingPathComponent("mod.json"))) as! [String: Any]
            let saved = (manifest["fomod_choices"] as! [[String: Any]]).map { ($0["step"] as! String, $0["group"] as! String, $0["plugins"] as! [String]) }
            try expectEqual(saved.count, chosen.selections.count)
            try expect(saved.contains { $0.1 == "Pack" && $0.2 == ["Hardcore"] })
            try expectEqual(two.id, "test-overhaul-2")
        }
    }

    test("fomod install: UTF-16 config in an archive, 7z, and a config that is broken falls back to plain install") {
        try await withTempDir { dir in
            let library = try ModLibrary(root: dir.appendingPathComponent("Mods"))
            let importer = ModImporter(library: library)
            let utf16Archive = dir.appendingPathComponent("Utf16 Overhaul.7z")
            try writeArchive(overhaulEntries(configData: utf16(overhaulConfig), infoData: utf16(overhaulInfo)), format: .sevenZip(compression: "lzma2"), to: utf16Archive)
            let plan = try await importer.inspect(utf16Archive)
            try expectEqual(plan.fomod?.steps.count, 4)
            try expectEqual(plan.displayName, "Test Overhaul")
            let mod = try await importer.install(plan, choices: nil) { _ in }
            try expect(InstalledModProbe(content: mod.contentURL).exists("gamedata/configs/w.ltx"))

            let broken = dir.appendingPathComponent("Broken Installer.zip")
            try writeArchive([
                FixtureEntry("Broken/fomod/ModuleConfig.xml", "<config><oops"),
                FixtureEntry("Broken/gamedata/scripts/b.script", "b"),
            ], to: broken)
            let brokenPlan = try await importer.inspect(broken)
            try expect(brokenPlan.fomod == nil)
            try expect(brokenPlan.warnings.contains { $0.contains("FOMOD installer could not be read") }, "\(brokenPlan.warnings)")
            let plain = try await importer.install(brokenPlan, choices: nil) { _ in }
            try expectEqual(InstalledModProbe(content: plain.contentURL).allFiles(), ["gamedata/scripts/b.script"])
        }
    }

    test("fomod install: files written for a Data folder land in gamedata, missing sources do not abort") {
        try await withTempDir { dir in
            let config = """
            <config><moduleName>Data style</moduleName>
              <requiredInstallFiles><folder source="Files" destination=""/><file source="nothere.txt" destination="x.txt"/></requiredInstallFiles>
            </config>
            """
            let archive = dir.appendingPathComponent("Data Style.zip")
            try writeArchive([
                FixtureEntry("fomod/ModuleConfig.xml", config),
                FixtureEntry("Files/scripts/a.script", "a"), FixtureEntry("Files/configs/c.ltx", "c"),
            ], to: archive)
            let library = try ModLibrary(root: dir.appendingPathComponent("Mods"))
            let importer = ModImporter(library: library)
            let mod = try await importer.install(try await importer.inspect(archive), choices: nil) { _ in }
            try expectEqual(InstalledModProbe(content: mod.contentURL).allFiles(), ["gamedata/configs/c.ltx", "gamedata/scripts/a.script"])
        }
    }

    test("fomod security: a path that leaves the mod in the config is refused") {
        try await withTempDir { dir in
            let importer = ModImporter(library: try ModLibrary(root: dir.appendingPathComponent("Mods")))
            for (name, xml) in [
                ("destination", "<config><requiredInstallFiles><file source=\"a.txt\" destination=\"../../evil.txt\"/></requiredInstallFiles></config>"),
                ("source", "<config><requiredInstallFiles><file source=\"..\\..\\etc\\passwd\" destination=\"x\"/></requiredInstallFiles></config>"),
            ] {
                let archive = dir.appendingPathComponent("evil-\(name).zip")
                try writeArchive([FixtureEntry("fomod/ModuleConfig.xml", xml), FixtureEntry("a.txt", "a"), FixtureEntry("gamedata/x.txt", "x")], to: archive)
                try await expectError(name, matches: { if case ModImportError.unsafePath = $0 { return true } else { return false } }) {
                    _ = try await importer.inspect(archive)
                }
            }
            try expect(!FileManager.default.fileExists(atPath: dir.deletingLastPathComponent().appendingPathComponent("evil.txt").path))
        }
    }

    test("wizard: renders offscreen as a native grouped form, on page 2 with the pad cursor, with a custom accent, in both colour schemes") {
        let installer = try installerFrom(Data(overhaulConfig.utf8))
        try await withTempDir { dir in
            let archive = dir.appendingPathComponent("Test Overhaul.zip")
            try writeArchive(overhaulEntries(configData: Data(overhaulConfig.utf8)), to: archive)
            let importer = ModImporter(library: try ModLibrary(root: dir.appendingPathComponent("Mods")))
            let withImages = try await importer.inspect(archive).fomod ?? installer
            let artifacts = ProcessInfo.processInfo.environment["MODMANAGER_TEST_ARTIFACTS"].map { URL(fileURLWithPath: $0) }

            try await MainActor.run {
                // The stock list and form are AppKit controls that ImageRenderer cannot draw, so the page is hosted in a window that
                // is never shown and drawn into a bitmap (the launcher's snapshot tool does the same).
                @MainActor func render<V: View>(_ name: String, _ size: CGSize, dark: Bool = true, glass: Bool = false, _ view: V) throws -> CGImage {
                    let host = NSHostingView(rootView: view.environment(\.fomodGlassDisabled, !glass).environment(\.controlActiveState, .key))
                    let window = NSWindow(contentRect: CGRect(origin: .zero, size: size), styleMask: [.titled], backing: .buffered, defer: false)
                    window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
                    window.contentView = host
                    host.frame = CGRect(origin: .zero, size: size)
                    for _ in 0..<5 {
                        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
                        host.layoutSubtreeIfNeeded()
                        host.displayIfNeeded()
                    }
                    guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { throw TestFailure(message: "no bitmap (\(name))") }
                    host.cacheDisplay(in: host.bounds, to: rep)
                    guard let image = rep.cgImage else { throw TestFailure(message: "wizard did not render (\(name))") }
                    try expect(!window.isVisible, "the window was never shown")
                    guard let data = image.dataProvider?.data, let bytes = CFDataGetBytePtr(data) else { throw TestFailure(message: "no pixels") }
                    var colors = Set<UInt32>()
                    var i = 0
                    while i + 3 < CFDataGetLength(data) && colors.count < 64 {
                        colors.insert(UInt32(bytes[i]) << 16 | UInt32(bytes[i + 1]) << 8 | UInt32(bytes[i + 2]))
                        i += 4 * 37
                    }
                    try expect(colors.count >= 8, "wizard render has content (\(name), \(colors.count) colors)")
                    if let artifacts {
                        try FileManager.default.createDirectory(at: artifacts, withIntermediateDirectories: true)
                        try rep.representation(using: .png, properties: [:])?.write(to: artifacts.appendingPathComponent("wizard-\(name).png"))
                    }
                    return image
                }
                func fingerprint(_ image: CGImage) -> Int {
                    var hasher = Hasher()
                    if let data = image.dataProvider?.data { hasher.combine(Data(bytes: CFDataGetBytePtr(data), count: CFDataGetLength(data))) }
                    return hasher.finalize()
                }

                let size = CGSize(width: 820, height: 900)
                let light = try render("light", size, dark: false, FomodWizardView(installer: withImages, onFinish: { _ in }, onCancel: {}))
                let dark = try render("dark", size, FomodWizardView(installer: withImages, onFinish: { _ in }, onCancel: {}))
                try expect(fingerprint(light) != fingerprint(dark), "both colour schemes render, differently")

                // page 2, driven the way a pad drives it: Next, then the cursor on the locked option
                let session = FomodSession(installer: withImages)
                session.send(.next)
                session.cursor = .option(ref(1, 0, 2))      // "Locked on": required
                session.showsCursor = true
                let padPage = try render("page2-pad-cursor", size, FomodWizardView(session: session, onFinish: { _ in }, onCancel: {}))
                try expect(fingerprint(padPage) != fingerprint(dark), "another page renders differently")

                // the cursor on the primary button
                let footerSession = FomodSession(installer: withImages)
                footerSession.cursor = .primary
                footerSession.showsCursor = true
                _ = try render("footer-cursor", size, FomodWizardView(session: footerSession, onFinish: { _ in }, onCancel: {}))

                // a host with its own Cancel asks for none; a bare window asks for one
                _ = try render("with-cancel", size, FomodWizardView(installer: withImages, showsCancel: true, onFinish: { _ in }, onCancel: {}))

                // Reduce Motion: the same page; the resting picture must still be complete
                _ = try render("reduced-motion", size, FomodWizardView(installer: withImages, onFinish: { _ in }, onCancel: {})
                    .environment(\.fomodReduceMotionOverride, true))

                // an injected theme: another accent and a backdrop of the host's own
                let blue = FomodTheme(accent: Color(red: 0.25, green: 0.55, blue: 1.0),
                                      backdrop: AnyView(LinearGradient(colors: [Color(red: 0.05, green: 0.08, blue: 0.20), .black],
                                                                       startPoint: .top, endPoint: .bottom)))
                let themed = try render("custom-theme", size, FomodWizardView(installer: withImages, onFinish: { _ in }, onCancel: {})
                    .fomodTheme(blue))
                try expect(fingerprint(themed) != fingerprint(dark), "the theme changes the render")
            }
        }
    }
}
