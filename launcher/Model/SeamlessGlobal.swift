// SPDX-License-Identifier: GPL-2.0-or-later
// BBSC v1.4.0 password-account protocol, adapted from Kaniel's bbport_auth.py.
import Foundation

enum SeamlessHost: String, CaseIterable, Identifiable {
    case local, global
    var id: String { rawValue }
    var label: String { self == .global ? "seamlesshunt.com (Global)" : "Private / local server" }
}

enum SeamlessGlobal {
    static let base = URL(string: "https://seamlesshunt.com")!
    static let endpoint = "seamlesshunt.com:31314"
    static let keychainService = "bbport-seamless-global"
    static func keychainAccount(_ id: String) -> String { "seamlesshunt.com:\(id)" }

    enum Action: String, CaseIterable, Identifiable {
        case signIn, create, recover, change
        var id: String { rawValue }
        var label: String {
            switch self {
            case .signIn: "Sign In"
            case .create: "Create Account"
            case .recover: "Recover Account"
            case .change: "Change Password"
            }
        }
    }
    struct Failure: LocalizedError {
        var message: String
        var errorDescription: String? { message }
    }
    struct Account: Decodable {
        var id: String
        var secret: String
        var name: String
        var recovery_code: String?

        var expires: TimeInterval {
            TimeInterval(UInt32(secret.prefix(8), radix: 16) ?? 0) + 29 * 24 * 3600
        }
        func validate() throws {
            guard id.range(of: "^u[0-9a-f]{14}$", options: .regularExpression) != nil,
                  secret.range(of: "^[0-9a-f]{32}$", options: .regularExpression) != nil,
                  expires > Date().timeIntervalSince1970 else {
                throw Failure(message: "The server returned an invalid or expired sign-in.")
            }
        }
    }
    static func validate(name: String, password: String?) throws {
        guard name.range(of: "^[A-Za-z0-9_.-]{3,20}$", options: .regularExpression) != nil else {
            throw Failure(message: "Use 3–20 letters, digits, underscores, dots or hyphens for the account name.")
        }
        if let password {
            guard (10...128).contains(password.count) else {
                throw Failure(message: "Use a password of 10–128 characters.")
            }
            guard !password.lowercased().contains(name.lowercased()) else {
                throw Failure(message: "The password must not contain your account name.")
            }
        }
    }
    static func request(path: String, body: [String: Any]?) throws -> URLRequest {
        var request = URLRequest(url: base.appendingPathComponent("bb/auth/" + path))
        request.timeoutInterval = 30
        request.httpMethod = body == nil ? "GET" : "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if let body { request.httpBody = try JSONSerialization.data(withJSONObject: body) }
        return request
    }
    static func refusal(status: Int, data: Data) -> Failure {
        let answer = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
        let error = answer["error"] as? String ?? ""
        if status == 401 { return Failure(message: "Wrong account name, password or recovery code.") }
        if status == 409 { return Failure(message: "That account name is taken.") }
        if status == 429, error == "wait" {
            return Failure(message: "Too many attempts. Try again in \(answer["seconds"] as? Int ?? 1) seconds.")
        }
        if status == 429, error == "signups" {
            return Failure(message: "Too many new accounts from this address. Try again in an hour.")
        }
        if status == 429 || status == 503 { return Failure(message: "The server is busy. Try again shortly.") }
        if status == 400, error == "password" {
            return Failure(message: "The server refused that password. Choose a longer, less common password without your account name.")
        }
        return Failure(message: "The server refused the request (HTTP \(status)).")
    }
}
