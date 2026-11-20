// Makes platendoos's app icon the way macOS 26 shows rekordbox's: rekordbox's Assets.car holds an Icon Composer
// icon (a background gradient and the logo as one SVG layer, with glass, shine and shadow drawn by macOS), not a
// picture. This takes that logo and those gradients, inverts the colours (light tile, black logo) and compiles the
// result with Xcode's actool, so the margins, glass and dark-mode look are exactly rekordbox's.
//   swift tools/build/layered_icon.swift <rekordbox's Assets.car> <Resources dir to write Assets.car and platendoos.icns to>
// Exits non-zero when it can't (no actool, unexpected Assets.car): build.sh then falls back to invert_icon.swift.
import Foundation

let args = CommandLine.arguments
guard args.count == 3 else { fatalError("usage: layered_icon.swift <Assets.car> <out dir>") }
let car = try! Data(contentsOf: URL(fileURLWithPath: args[1]))

func fail(_ why: String) -> Never {
    FileHandle.standardError.write("layered_icon: \(why)\n".data(using: .utf8)!)
    exit(1)
}

func run(_ tool: String, _ a: [String]) -> Data {
    let p = Process(), pipe = Pipe()
    p.executableURL = URL(fileURLWithPath: tool)
    p.arguments = a
    p.standardOutput = pipe
    p.standardError = FileHandle.nullDevice
    do { try p.run() } catch { fail("can't run \(tool)") }
    let out = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    if p.terminationStatus != 0 { fail("\(tool) \(a.first ?? "") failed") }
    return out
}

// The logo: the LZFSE block (magic "bvx") that decodes to an SVG.
var svg: String?
let magic = "bvx".data(using: .ascii)!
var from = car.startIndex
while svg == nil, let r = car.range(of: magic, in: from..<car.endIndex) {
    if let d = try? (car[r.lowerBound...] as NSData).decompressed(using: .lzfse) as Data,
       let s = String(data: d, encoding: .utf8), s.hasPrefix("<svg") {
        svg = s
    }
    from = r.upperBound
}
guard var logo = svg else { fail("no SVG layer in Assets.car") }
guard logo.contains("fill=\"white\"/>") else { fail("the logo isn't white any more") }
logo = logo.replacingOccurrences(of: "fill=\"white\"/>", with: "fill=\"black\"/>")  // the paths, not the clip rect

// The background gradients (light and dark appearance) and the layer's offset, from assetutil.
let info = try! JSONSerialization.jsonObject(with: run("/usr/bin/assetutil", ["--info", args[1]])) as! [Any]
let renditions = info.dropFirst().compactMap { $0 as? [String: Any] }
var colors: [String: (space: String, c: [Double])] = [:]
var gradients: [String: [String]] = [:]
var position = "0,0"
for r in renditions {
    let name = (r["Name"] as? String ?? "").components(separatedBy: "/").last ?? ""
    switch r["AssetType"] as? String {
    case "Color": colors[name] = (r["Colorspace"] as? String ?? "", r["Color components"] as? [Double] ?? [])
    case "Named Gradient": gradients[name] = (r["Gradient Colors"] as? [String] ?? []).map { $0.components(separatedBy: "/").last! }
    case "IconGroup": position = ((r["Layers"] as? [[String: Any]])?.first?["LayerPosition"] as? String) ?? position
    default: break
    }
}
// Which gradient is the dark appearance's: the IconImageStack whose Appearance is DarkAqua names it as its first layer.
func gradientOf(_ appearance: String) -> String? {
    let stack = renditions.first { $0["AssetType"] as? String == "IconImageStack" && $0["Appearance"] as? String == appearance }
    return ((stack?["Layers"] as? [[String: Any]])?.first?["Name"] as? String)?.components(separatedBy: "/").last
}
// One gradient's colours, inverted, in Icon Composer's notation.
func inverted(_ gradient: String?) -> String? {
    guard let g = gradient, let names = gradients[g], names.count == 2 else { return nil }
    let parts = names.compactMap { n -> String? in
        guard let (space, c) = colors[n], let alpha = c.last else { return nil }
        let inv = c.dropLast().map { String(format: "%.5f", 1 - $0) }.joined(separator: ",")
        let a = String(format: "%.5f", alpha)
        switch space {
        case "p3": return "\"display-p3:\(inv),\(a)\""
        case "srgb": return "\"srgb:\(inv),\(a)\""
        case "gray gamma 22", "extended gray": return "\"gray:\(inv),\(a)\""
        default: return nil
        }
    }
    return parts.count == 2 ? "{ \"linear-gradient\" : [ \(parts.joined(separator: ", ")) ] }" : nil
}
guard let light = inverted(gradientOf("NSAppearanceNameAqua")), let dark = inverted(gradientOf("NSAppearanceNameDarkAqua"))
else { fail("unexpected background in Assets.car") }
let xy = position.components(separatedBy: ",").compactMap { Double($0) }
guard xy.count == 2 else { fail("unexpected layer position \(position)") }

// rekordbox's settings otherwise: glass with shine, neutral shadow at 0.5, translucency 0.3 (checked: actool makes
// the same Assets.car from this with rekordbox's own colours).
let json = """
{
  "fill-specializations" : [ { "value" : \(light) }, { "appearance" : "dark", "value" : \(dark) } ],
  "groups" : [
    {
      "layers" : [ { "glass" : true, "image-name" : "logo.svg", "name" : "logo",
                     "position" : { "scale" : 1, "translation-in-points" : [ \(xy[0]), \(xy[1]) ] } } ],
      "shadow" : { "kind" : "neutral", "opacity" : 0.5 },
      "specular" : true,
      "translucency" : { "enabled" : true, "value" : 0.3 }
    }
  ],
  "supported-platforms" : { "squares" : [ "macOS" ] }
}
"""

let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("platendoos-icon-\(getpid())")
let icon = tmp.appendingPathComponent("platendoos.icon")
try? FileManager.default.removeItem(at: tmp)
try! FileManager.default.createDirectory(at: icon.appendingPathComponent("Assets"), withIntermediateDirectories: true)
try! logo.write(to: icon.appendingPathComponent("Assets/logo.svg"), atomically: true, encoding: .utf8)
try! json.write(to: icon.appendingPathComponent("icon.json"), atomically: true, encoding: .utf8)
let built = tmp.appendingPathComponent("out")
try! FileManager.default.createDirectory(at: built, withIntermediateDirectories: true)
_ = run("/usr/bin/xcrun", ["actool", icon.path, "--compile", built.path, "--platform", "macosx",
                           "--minimum-deployment-target", "26.0", "--app-icon", "platendoos",
                           "--output-partial-info-plist", tmp.appendingPathComponent("partial.plist").path])
let dest = URL(fileURLWithPath: args[2])
for f in ["Assets.car", "platendoos.icns"] {
    let src = built.appendingPathComponent(f)
    guard FileManager.default.fileExists(atPath: src.path) else { fail("actool made no \(f)") }
    try? FileManager.default.removeItem(at: dest.appendingPathComponent(f))
    try! FileManager.default.copyItem(at: src, to: dest.appendingPathComponent(f))
}
try? FileManager.default.removeItem(at: tmp)
