// validate_primitives.swift — batch-validate the two remaining
// verusclhash primitives (mulhrs_epi16 + precompReduction64) on GPU
// vs the C++ references. clmul64 already validated in its own harness.
//
// With this passing, every cryptographic building block verusclhash_sv2_2
// needs is on the GPU and proven byte-perfect against CPU. The only
// remaining work to make a full GPU verusclhash is the 32-iteration
// selector loop body, which is straight translation of the 8 switch cases
// (each is some combination of clmul + mulhrs + xor + load + store, plus
// cases 0x10/0x14/0x18 also use the haraka512_keyed AES rounds I already
// ported).
//
// Compile:
//   cd verusminer/metal
//   clang++ -c -std=c++17 -O2 -I../cpu/canonical -I../cpu \
//       ../cpu/canonical/clmul_shim.cpp -o ../cpu/canonical/clmul_shim.o
//   swiftc -O validate_primitives.swift -o validate_primitives \
//       -framework Metal -framework Foundation \
//       ../cpu/canonical/clmul_shim.o \
//       ../cpu/canonical/verus_clhash_portable.o \
//       ../cpu/haraka_portable.o \
//       -Xlinker -lc++ -Xlinker -lm

import Foundation
import Metal

// ---- CPU references ----
@_silgen_name("mulhrs_epi16_wrap")
func mulhrs_epi16_wrap(_ a: UnsafePointer<UInt8>,
                       _ b: UnsafePointer<UInt8>,
                       _ out: UnsafeMutablePointer<UInt8>)

@_silgen_name("precomp_reduction64_wrap")
func precomp_reduction64_wrap(_ a: UnsafePointer<UInt8>,
                              _ out: UnsafeMutablePointer<UInt8>)

// ---- GPU setup ----
let kernelFile = "verusclhash_primitives.metal"
let kernelPath = FileManager.default.currentDirectoryPath + "/" + kernelFile
print("Loading kernel: \(kernelFile)")
guard let kernelSrc = try? String(contentsOfFile: kernelPath, encoding: .utf8) else {
    fatalError("Cannot load \(kernelFile) from cwd")
}
guard let device = MTLCreateSystemDefaultDevice() else { fatalError("No Metal device") }
guard let queue = device.makeCommandQueue() else { fatalError("No queue") }
print("GPU: \(device.name)")

let lib: MTLLibrary
do { lib = try device.makeLibrary(source: kernelSrc, options: nil) }
catch { fatalError("Kernel compile failed: \(error)") }
guard let fnMulhrs = lib.makeFunction(name: "mulhrs_kernel") else { fatalError("mulhrs_kernel?") }
guard let fnReduc  = lib.makeFunction(name: "precomp_reduction64_kernel") else { fatalError("precomp_reduction64_kernel?") }
let psMulhrs = try! device.makeComputePipelineState(function: fnMulhrs)
let psReduc  = try! device.makeComputePipelineState(function: fnReduc)

// ---- Generator: deterministic 16-byte vectors ----
var rngState: UInt64 = 0xC0FFEE_C0FFEE_C0FF
func xs64() -> UInt64 {
    rngState ^= rngState >> 12
    rngState ^= rngState &<< 25
    rngState ^= rngState >> 27
    return rngState &* 0x2545F4914F6CDD1D
}

func rand16() -> [UInt8] {
    let v1 = xs64(), v2 = xs64()
    var b = [UInt8](repeating: 0, count: 16)
    for i in 0..<8 { b[i]     = UInt8((v1 >> (8 * i)) & 0xff) }
    for i in 0..<8 { b[i + 8] = UInt8((v2 >> (8 * i)) & 0xff) }
    return b
}

func hex(_ b: [UInt8]) -> String {
    return b.map { String(format: "%02x", $0) }.joined()
}

// ============================================================
// Test 1: mulhrs_epi16
// ============================================================
print("\n=== mulhrs_epi16 ===")

// Edge cases that stress sign / rounding / overflow paths
let mulhrsEdges: [(String, [UInt8], [UInt8])] = [
    ("zero × zero",
     Array(repeating: 0, count: 16),
     Array(repeating: 0, count: 16)),
    ("max+ × max+",  // 0x7fff per lane = max positive int16
     Array(repeating: [0xff, 0x7f], count: 8).flatMap { $0 },
     Array(repeating: [0xff, 0x7f], count: 8).flatMap { $0 }),
    ("max- × max-",  // 0x8000 per lane = -32768
     Array(repeating: [0x00, 0x80], count: 8).flatMap { $0 },
     Array(repeating: [0x00, 0x80], count: 8).flatMap { $0 }),
    ("max+ × max-",
     Array(repeating: [0xff, 0x7f], count: 8).flatMap { $0 },
     Array(repeating: [0x00, 0x80], count: 8).flatMap { $0 }),
    ("one × max+",   // 0x0001 × 0x7fff per lane
     Array(repeating: [0x01, 0x00], count: 8).flatMap { $0 },
     Array(repeating: [0xff, 0x7f], count: 8).flatMap { $0 }),
]

var pairs: [(String, [UInt8], [UInt8])] = mulhrsEdges
for i in 0..<512 {
    pairs.append(("rand[\(i)]", rand16(), rand16()))
}

// CPU pass
var cpuOuts: [[UInt8]] = []
for (_, a, b) in pairs {
    var out = [UInt8](repeating: 0, count: 16)
    a.withUnsafeBufferPointer { ap in
        b.withUnsafeBufferPointer { bp in
            out.withUnsafeMutableBufferPointer { op in
                mulhrs_epi16_wrap(ap.baseAddress!, bp.baseAddress!, op.baseAddress!)
            }
        }
    }
    cpuOuts.append(out)
}

// GPU batch: pack (a, b) pairs as 32-byte stride
var inBytes = [UInt8](repeating: 0, count: pairs.count * 32)
for (i, (_, a, b)) in pairs.enumerated() {
    for j in 0..<16 { inBytes[i * 32 + j]      = a[j] }
    for j in 0..<16 { inBytes[i * 32 + 16 + j] = b[j] }
}
let mhInBuf = inBytes.withUnsafeBufferPointer { p in
    device.makeBuffer(bytes: p.baseAddress!, length: p.count, options: .storageModeShared)!
}
let mhOutBuf = device.makeBuffer(length: pairs.count * 16, options: .storageModeShared)!

let cb1 = queue.makeCommandBuffer()!
let enc1 = cb1.makeComputeCommandEncoder()!
enc1.setComputePipelineState(psMulhrs)
enc1.setBuffer(mhInBuf,  offset: 0, index: 0)
enc1.setBuffer(mhOutBuf, offset: 0, index: 1)
let tpg1 = min(psMulhrs.maxTotalThreadsPerThreadgroup, 256)
enc1.dispatchThreads(MTLSize(width: pairs.count, height: 1, depth: 1),
                     threadsPerThreadgroup: MTLSize(width: tpg1, height: 1, depth: 1))
enc1.endEncoding()
cb1.commit()
cb1.waitUntilCompleted()
let gpuPtr1 = mhOutBuf.contents().bindMemory(to: UInt8.self, capacity: pairs.count * 16)
let gpuOut1Flat = Array(UnsafeBufferPointer(start: gpuPtr1, count: pairs.count * 16))

var mhFails = 0
for (i, _) in pairs.enumerated() {
    let g = Array(gpuOut1Flat[(i * 16)..<((i + 1) * 16)])
    if g != cpuOuts[i] {
        if mhFails < 3 {
            print("  ✗ \(pairs[i].0): CPU=\(hex(cpuOuts[i])) GPU=\(hex(g))")
        }
        mhFails += 1
    }
}
for i in 0..<5 {
    let g = Array(gpuOut1Flat[(i * 16)..<((i + 1) * 16)])
    let ok = g == cpuOuts[i]
    print("  [\(pairs[i].0)] CPU=\(hex(cpuOuts[i])) GPU=\(hex(g)) \(ok ? "✓" : "✗")")
}
print("  mulhrs: \(pairs.count - mhFails)/\(pairs.count) PASS")

// ============================================================
// Test 2: precompReduction64
// ============================================================
print("\n=== precompReduction64 ===")

let redEdges: [(String, [UInt8])] = [
    ("zero",       Array(repeating: 0, count: 16)),
    ("one-low",    [1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0]),
    ("one-high",   [0,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0]),
    ("all-ones",   Array(repeating: 0xff, count: 16)),
    ("msb-set",    Array(repeating: 0x80, count: 16)),
    ("counting",   (0..<16).map { UInt8($0) }),
]

var redInputs: [(String, [UInt8])] = redEdges
for i in 0..<512 {
    redInputs.append(("rand[\(i)]", rand16()))
}

// CPU pass
var redCPUOuts: [[UInt8]] = []
for (_, a) in redInputs {
    var out = [UInt8](repeating: 0, count: 16)
    a.withUnsafeBufferPointer { ap in
        out.withUnsafeMutableBufferPointer { op in
            precomp_reduction64_wrap(ap.baseAddress!, op.baseAddress!)
        }
    }
    redCPUOuts.append(out)
}

// GPU batch
var redInBytes = [UInt8](repeating: 0, count: redInputs.count * 16)
for (i, (_, a)) in redInputs.enumerated() {
    for j in 0..<16 { redInBytes[i * 16 + j] = a[j] }
}
let redInBuf = redInBytes.withUnsafeBufferPointer { p in
    device.makeBuffer(bytes: p.baseAddress!, length: p.count, options: .storageModeShared)!
}
let redOutBuf = device.makeBuffer(length: redInputs.count * 16, options: .storageModeShared)!

let cb2 = queue.makeCommandBuffer()!
let enc2 = cb2.makeComputeCommandEncoder()!
enc2.setComputePipelineState(psReduc)
enc2.setBuffer(redInBuf,  offset: 0, index: 0)
enc2.setBuffer(redOutBuf, offset: 0, index: 1)
let tpg2 = min(psReduc.maxTotalThreadsPerThreadgroup, 256)
enc2.dispatchThreads(MTLSize(width: redInputs.count, height: 1, depth: 1),
                     threadsPerThreadgroup: MTLSize(width: tpg2, height: 1, depth: 1))
enc2.endEncoding()
cb2.commit()
cb2.waitUntilCompleted()
let gpuPtr2 = redOutBuf.contents().bindMemory(to: UInt8.self, capacity: redInputs.count * 16)
let gpuOut2Flat = Array(UnsafeBufferPointer(start: gpuPtr2, count: redInputs.count * 16))

var redFails = 0
for (i, _) in redInputs.enumerated() {
    let g = Array(gpuOut2Flat[(i * 16)..<((i + 1) * 16)])
    // Per CPU reference: "high 64 bits should be assumed to contain garbage"
    // Both sides compute garbage the same way (same algorithm), so the FULL
    // 16 bytes should still match byte-for-byte. If they don't, the reduction
    // is wrong somewhere even if the low 8 happen to agree.
    if g != redCPUOuts[i] {
        if redFails < 3 {
            print("  ✗ \(redInputs[i].0):")
            print("      CPU=\(hex(redCPUOuts[i]))")
            print("      GPU=\(hex(g))")
        }
        redFails += 1
    }
}
for i in 0..<6 {
    let g = Array(gpuOut2Flat[(i * 16)..<((i + 1) * 16)])
    let ok = g == redCPUOuts[i]
    print("  [\(redInputs[i].0)] CPU=\(hex(redCPUOuts[i])) GPU=\(hex(g)) \(ok ? "✓" : "✗")")
}
print("  precompReduction64: \(redInputs.count - redFails)/\(redInputs.count) PASS")

print("")
if mhFails == 0 && redFails == 0 {
    print("PASS — both primitives byte-perfect on \(pairs.count + redInputs.count) total vectors")
    exit(0)
} else {
    print("FAIL — mulhrs:\(mhFails) reduction:\(redFails)")
    exit(1)
}
