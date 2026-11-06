// SPDX-License-Identifier: GPL-2.0-or-later
// Native macOS HTTPS transport for BBSC v1.4.0's bbport_auth.py protocol.
import CryptoKit
import Foundation

private final class NoAuthRedirects: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

enum SeamlessGlobalClient {
    private static let redirects = NoAuthRedirects()
    private static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.httpShouldSetCookies = false
        config.urlCache = nil
        return URLSession(configuration: config, delegate: redirects, delegateQueue: nil)
    }()

    private static func call(_ path: String, _ body: [String: Any]? = nil) async throws -> Data {
        let request = try SeamlessGlobal.request(path: path, body: body)
        let (data, response) = try await session.data(for: request)
        guard let response = response as? HTTPURLResponse, response.statusCode == 200 else {
            throw SeamlessGlobal.refusal(status: (response as? HTTPURLResponse)?.statusCode ?? 0, data: data)
        }
        return data
    }

    static func authenticate(action: SeamlessGlobal.Action, name: String, password: String,
                             newPassword: String, recoveryCode: String) async throws -> SeamlessGlobal.Account {
        try SeamlessGlobal.validate(name: name, password: action == .create ? password :
                                    (action == .recover || action == .change ? newPassword : nil))
        var body: [String: Any] = ["name": name]
        let path: String
        switch action {
        case .signIn:
            path = "sign-in"; body["password"] = password
        case .create:
            struct Challenge: Decodable { var challenge: String; var bits: Int }
            let challenge = try JSONDecoder().decode(Challenge.self, from: await call("challenge", [:]))
            guard (0...28).contains(challenge.bits), !challenge.challenge.isEmpty else {
                throw SeamlessGlobal.Failure(message: "The server requested unsupported account work.")
            }
            let nonce = try await Task.detached(priority: .userInitiated) {
                try solve(challenge: challenge.challenge, name: name, bits: challenge.bits)
            }.value
            path = "sign-up"; body["password"] = password
            body["challenge"] = challenge.challenge; body["nonce"] = nonce
        case .recover:
            path = "recover"; body["recovery_code"] = recoveryCode; body["new_password"] = newPassword
        case .change:
            guard password != newPassword else {
                throw SeamlessGlobal.Failure(message: "Choose a new password different from the old one.")
            }
            path = "change"; body["password"] = password; body["new_password"] = newPassword
        }
        let account = try JSONDecoder().decode(SeamlessGlobal.Account.self, from: await call(path, body))
        try account.validate()
        return account
    }

    static func signOut(id: String, secret: String) async throws {
        _ = try await call("sign-out", ["id": id, "secret": secret])
    }

    // Same proof of work as PasswordAccounts::WorkDone and bbport_auth.solve.
    static func solve(challenge: String, name: String, bits: Int) throws -> UInt64 {
        guard (0...28).contains(bits) else {
            throw SeamlessGlobal.Failure(message: "Unsupported account work.")
        }
        var prefix = SHA256()
        prefix.update(data: Data("bb signup v1|\(challenge)|\(name.lowercased())|".utf8))
        var nonce: UInt64 = 0
        while true {
            if nonce % 65536 == 0 { try Task.checkCancellation() }
            var hash = prefix
            var little = nonce.littleEndian
            withUnsafeBytes(of: &little) { hash.update(bufferPointer: $0) }
            let digest = Array(hash.finalize())
            let bytes = bits / 8, remainder = bits % 8
            if digest.prefix(bytes).allSatisfy({ $0 == 0 }) &&
                (remainder == 0 || digest[bytes] >> (8 - remainder) == 0) { return nonce }
            nonce += 1
        }
    }
}
