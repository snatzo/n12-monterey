// pdftext.swift - pull text out of a PDF using macOS's built-in PDFKit.
//
// Why: the n8/n12 service manual is a 245-page PDF and none of the usual
// command-line tools (pdftotext, mutool, qpdf) are installed. PDFKit is part of
// macOS, so a few lines of Swift gives us what we need without installing
// anything.
//
// Usage:
//   swift tools/pdftext.swift summary  <file.pdf>
//   swift tools/pdftext.swift page     <file.pdf> <page-number>
//   swift tools/pdftext.swift dump     <file.pdf> [start] [end]
//   swift tools/pdftext.swift find     <file.pdf> <regex>
//
// `summary` is the useful first step: it prints, for every page, the number of
// characters of text found and the first line. Pages with 0 characters are
// scanned images with no text layer, which tells us whether OCR is needed and
// which pages are worth rendering as images.

import Foundation
import AppKit
import PDFKit

// Print usage and exit. Called when arguments are wrong or missing.
func usage() -> Never {
    FileHandle.standardError.write(
        "usage: pdftext.swift summary|page|dump|find <file.pdf> [args]\n".data(using: .utf8)!
    )
    exit(2)
}

let args = CommandLine.arguments
// args[0] is the script path; mode and filename follow.
guard args.count >= 3 else { usage() }
let mode = args[1]
let path = args[2]

// PDFDocument parses the file lazily; `nil` means it isn't a readable PDF.
guard let doc = PDFDocument(url: URL(fileURLWithPath: path)) else {
    FileHandle.standardError.write("cannot open PDF: \(path)\n".data(using: .utf8)!)
    exit(1)
}

// Text of a single page, or "" for image-only pages.
func text(at index: Int) -> String {
    guard let page = doc.page(at: index) else { return "" }
    return page.string ?? ""
}

// Left-pad a string to a fixed width so columns line up. Done by hand rather
// than with String(format:) because "%s" there expects a C string, not a Swift
// String, and passing one crashes.
func lpad(_ s: String, _ width: Int) -> String {
    s.count >= width ? s : String(repeating: " ", count: width - s.count) + s
}

// Render one page to a PNG. The block diagrams and schematics are vector
// drawings whose real content (arrows, tap points) does not survive text
// extraction, so some questions can only be answered by looking at the page.
// `scale` multiplies the PDF's own point size, so 2.0 is roughly 144 dpi.
// If `crop` is given (in PDF points, origin bottom-left, as PDFKit uses), only
// that rectangle is rendered - which is how the dense schematics get read.
func render(page index: Int, scale: CGFloat, crop: CGRect?, to outputPath: String) -> Bool {
    guard let page = doc.page(at: index) else { return false }
    let full = page.bounds(for: .mediaBox)
    // The region we actually draw: either the crop, or the whole page.
    let region = crop ?? full
    let pixelSize = CGSize(width: region.width * scale, height: region.height * scale)

    // Draw into an offscreen image, white background so the drawing is legible.
    let image = NSImage(size: pixelSize)
    image.lockFocus()
    NSColor.white.setFill()
    NSRect(origin: .zero, size: pixelSize).fill()
    guard let ctx = NSGraphicsContext.current?.cgContext else {
        image.unlockFocus()
        return false
    }
    ctx.scaleBy(x: scale, y: scale)
    // Shift the page so `region`'s bottom-left corner lands at the image origin,
    // then clip to it, so everything outside the crop is discarded.
    ctx.translateBy(x: -region.origin.x, y: -region.origin.y)
    ctx.clip(to: region)
    page.draw(with: .mediaBox, to: ctx)
    image.unlockFocus()

    guard let tiff = image.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else { return false }
    return (try? png.write(to: URL(fileURLWithPath: outputPath))) != nil
}

switch mode {
case "render":
    // Usage: render <pdf> <page> <scale> <out.png> [x y w h]
    // The optional four numbers crop to that rectangle, in PDF points.
    guard args.count >= 6, let n = Int(args[3]) else { usage() }
    let scale = Double(args[4]) ?? 2.0
    let out = args[5]
    var crop: CGRect? = nil
    if args.count >= 10,
       let x = Double(args[6]), let y = Double(args[7]),
       let w = Double(args[8]), let h = Double(args[9]) {
        crop = CGRect(x: x, y: y, width: w, height: h)
    }
    if render(page: n - 1, scale: CGFloat(scale), crop: crop, to: out) {
        // Report the resulting file size so the caller knows what they got.
        let attrs = try? FileManager.default.attributesOfItem(atPath: out)
        let size = (attrs?[.size] as? Int) ?? 0
        print("wrote \(out) (\(size) bytes)")
    } else {
        FileHandle.standardError.write("render failed\n".data(using: .utf8)!)
        exit(1)
    }

case "summary":
    print("file:  \(path)")
    print("pages: \(doc.pageCount)")
    print("")
    print("\(lpad("page", 5))  \(lpad("chars", 7))  first line")
    for i in 0..<doc.pageCount {
        let t = text(at: i)
        let trimmed = t.trimmingCharacters(in: .whitespacesAndNewlines)
        // First non-empty line makes a decent heading for the page.
        let firstLine = trimmed
            .components(separatedBy: .newlines)
            .first { !$0.trimmingCharacters(in: .whitespaces).isEmpty } ?? ""
        let shown = firstLine.count > 70 ? String(firstLine.prefix(70)) + "..." : firstLine
        print("\(lpad(String(i + 1), 5))  \(lpad(String(t.count), 7))  \(shown)")
    }

case "page":
    guard args.count >= 4, let n = Int(args[3]) else { usage() }
    // Pages are 1-based on the command line, 0-based in PDFKit.
    print(text(at: n - 1))

case "dump":
    let start = args.count >= 4 ? max(1, Int(args[3]) ?? 1) : 1
    let end = args.count >= 5 ? min(doc.pageCount, Int(args[4]) ?? doc.pageCount) : doc.pageCount
    for i in (start - 1)..<end {
        // Page markers make the dumped text greppable and citable.
        print("\n===== PAGE \(i + 1) =====")
        print(text(at: i))
    }

case "find":
    guard args.count >= 4 else { usage() }
    let pattern = args[3]
    guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else {
        FileHandle.standardError.write("bad regex\n".data(using: .utf8)!)
        exit(1)
    }
    for i in 0..<doc.pageCount {
        let t = text(at: i)
        let range = NSRange(t.startIndex..., in: t)
        if re.firstMatch(in: t, range: range) != nil {
            // Show which page matched and the surrounding lines.
            print("----- PAGE \(i + 1)")
            for line in t.components(separatedBy: .newlines) {
                let lr = NSRange(line.startIndex..., in: line)
                if re.firstMatch(in: line, range: lr) != nil {
                    print("  \(line.trimmingCharacters(in: .whitespaces))")
                }
            }
        }
    }

default:
    usage()
}
