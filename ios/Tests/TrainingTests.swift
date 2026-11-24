import XCTest

final class TrainingTests: XCTestCase {
    func testSummonResumeAndAdvance() {
        continueAfterFailure = false
        let app = XCUIApplication()
        // Override the launch level for this process without clearing player progress.
        app.launchArguments = ["-lastLevel", "11"]
        XCUIDevice.shared.orientation = .portrait
        defer { XCUIDevice.shared.orientation = .portrait }
        app.launch()
        XCTAssertTrue(app.buttons["First Drop"].waitForExistence(timeout: 20))
        XCUIDevice.shared.press(.home)
        app.activate()
        let resume = app.buttons["Resume"]
        XCTAssertTrue(resume.waitForExistence(timeout: 10))
        resume.tap()
        let field = app.descendants(matching: .any)["Battlefield"]
        placeSummon(on: field, eastWall: 720, x: 460, y: 128)
        let next = app.buttons["Next lesson"]
        XCTAssertTrue(next.waitForExistence(timeout: 12))
        next.tap()
        XCTAssertTrue(app.buttons["In the Shadows"].waitForExistence(timeout: 5))
        XCUIDevice.shared.orientation = .landscapeLeft
        placeSummon(on: field, eastWall: 480, x: 304, y: 96)
        XCTAssertTrue(next.waitForExistence(timeout: 12))
    }

    // Project the authored ring through the training camera into the current drawable.
    private func placeSummon(on field: XCUIElement, eastWall: Double, x: Double, y: Double) {
        let aspect = field.frame.width / field.frame.height
        let yaw = (aspect < 1 ? 72.0 : 45.0) * .pi / 180
        let pitch = 62.0 * .pi / 180
        let width = eastWall * cos(yaw) + 224 * sin(yaw)
        let height = (eastWall * sin(yaw) + 224 * cos(yaw)) * sin(pitch)
        let fovy = max((width + 44) / aspect, height + 104)
        let dx = x - eastWall / 2, dy = y - 112
        let px = 0.5 + (dx * cos(yaw) + dy * sin(yaw)) / (fovy * aspect)
        let py = 0.5 + ((dx * sin(yaw) - dy * cos(yaw)) * sin(pitch) + 27 * cos(pitch)) / fovy
        field.coordinate(withNormalizedOffset: CGVector(dx: px, dy: py)).tap()
    }
}
