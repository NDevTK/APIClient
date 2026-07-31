/*---
description: V8 mjsunit harmony/regexp-named-captures.js, under forced time-travel
---*/
// Copyright 2008 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

var MjsUnitAssertionError = class MjsUnitAssertionError {
  #cached_message = undefined;
  #message_func = undefined;

  constructor(message_func) {
    this.#message_func = message_func;
    // Temporarily install a custom stack trace formatter and restore the
    // previous value.
    let prevPrepareStackTrace = Error.prepareStackTrace;
    try {
      Error.prepareStackTrace = MjsUnitAssertionError.prepareStackTrace;
      // This allows fetching the stack trace using TryCatch::StackTrace.
      this.stack = new Error("MjsUnitAssertionError").stack;
    } finally {
      Error.prepareStackTrace = prevPrepareStackTrace;
    }
  }

  get message() {
    if (this.#cached_message === undefined) {
      this.#cached_message = this.#message_func();
    }
    return this.#cached_message
  }

  toString() {
    return this.message + "\n\nStack: " + this.stack;
  };
}

/*
 * This file is included in all mini jsunit test cases.  The test
 * framework expects lines that signal failed tests to start with
 * the f-word and ignore all other lines.
 */

// Expected and found values the same objects, or the same primitive
// values.
// For known primitive values, please use assertEquals.
var assertSame;

// Inverse of assertSame.
var assertNotSame;

// Expected and found values are identical primitive values or functions
// or similarly structured objects (checking internal properties
// of, e.g., Number and Date objects, the elements of arrays
// and the properties of non-Array objects).
var assertEquals;

// Deep equality predicate used by assertEquals.
var deepEquals;

// Expected and found values are not identical primitive values or functions
// or similarly structured objects (checking internal properties
// of, e.g., Number and Date objects, the elements of arrays
// and the properties of non-Array objects).
var assertNotEquals;

// The difference between expected and found value is within certain tolerance.
var assertEqualsDelta;

// The found object is an Array with the same length and elements
// as the expected object. The expected object doesn't need to be an Array,
// as long as it's "array-ish".
var assertArrayEquals;

// The found object must have the same enumerable properties as the
// expected object. The type of object isn't checked.
var assertPropertiesEqual;

// Assert that the string conversion of the found value is equal to
// the expected string. Only kept for backwards compatibility, please
// check the real structure of the found value.
var assertToStringEquals;

// Checks that the found value is true. Use with boolean expressions
// for tests that doesn't have their own assertXXX function.
var assertTrue;

// Checks that the found value is false.
var assertFalse;

// Checks that the found value is null. Kept for historical compatibility,
// please just use assertEquals(null, expected).
var assertNull;

// Checks that the found value is *not* null.
var assertNotNull;

// Assert that the passed function or eval code throws an exception.
// The optional second argument is an exception constructor that the
// thrown exception is checked against with "instanceof".
// The optional third argument is a message type string or RegExp object that is
// compared to the message of the thrown exception.
var assertThrows;

// Asserts that the found value is an exception of specific type with a specific
// error message. The optional second argument is an exception constructor that
// the thrown exception is checked against with "instanceof". The optional third
// argument is a message type string or RegExp object that is compared to the
// message of the thrown exception.
var assertException;

// Assert that the passed function throws an exception.
// The exception is checked against the second argument using assertEquals.
var assertThrowsEquals;

// Assert that the passed promise does not resolve, but eventually throws an
// exception. The optional second argument is an exception constructor that the
// thrown exception is checked against with "instanceof".
// The optional third argument is a message type string or RegExp object that is
// compared to the message of the thrown exception.
var assertThrowsAsync;

// Assert that the passed function or eval code does not throw an exception.
var assertDoesNotThrow;

// Assert that the passed code throws an early error (i.e. throws a SyntaxError
// at parse time).
var assertEarlyError;

// Assert that the passed code throws an exception when executed.
// Fails if the passed code throws an exception at parse time.
var assertThrowsAtRuntime;

// Asserts that the found value is an instance of the constructor passed
// as the second argument.
var assertInstanceof;

// Assert that this code is never executed (i.e., always fails if executed).
var assertUnreachable;

// Assert that the function code is (not) optimized.
// Only works with --allow-natives-syntax.
var assertOptimized;
var assertUnoptimized;

// Assert that a string contains another expected substring.
var assertContains;

// Assert that a string matches a given regex.
var assertMatches;

// Assert that a promise resolves or rejects.
// Parameters:
// {promise} - the promise
// {success} - optional - a callback which is called with the result of the
//             resolving promise.
//  {fail} -   optional - a callback which is called with the result of the
//             rejecting promise. If the promise is rejected but no {fail}
//             callback is set, the error is propagated out of the promise
//             chain.
var assertPromiseResult;

var promiseTestChain;

// These bits must be in sync with bits defined in Runtime_GetOptimizationStatus
var V8OptimizationStatus = {
  kIsFunction: 1 << 0,
  kNeverOptimize: 1 << 1,
  kMaybeDeopted: 1 << 2,
  kOptimized: 1 << 3,
  kMaglevved: 1 << 4,
  kTurboFanned: 1 << 5,
  kInterpreted: 1 << 6,
  kMarkedForOptimization: 1 << 7,
  kMarkedForConcurrentOptimization: 1 << 8,
  kOptimizingConcurrently: 1 << 9,
  kIsExecuting: 1 << 10,
  kTopmostFrameIsTurboFanned: 1 << 11,
  kLiteMode: 1 << 12,
  kMarkedForDeoptimization: 1 << 13,
  kBaseline: 1 << 14,
  kTopmostFrameIsInterpreted: 1 << 15,
  kTopmostFrameIsBaseline: 1 << 16,
  kIsLazy: 1 << 17,
  kTopmostFrameIsMaglev: 1 << 18,
  kOptimizeOnNextCallOptimizesToMaglev: 1 << 19,
  kOptimizeMaglevOptimizesToTurbofan: 1 << 20,
  kMarkedForMagkevOptimization: 1 << 21,
  kMarkedForConcurrentMaglevOptimization: 1 << 22,
};

// Returns true if --lite-mode is on and we can't ever turn on optimization.
var isNeverOptimizeLiteMode;

// Returns true if --no-turbofan mode is on.
var isNeverOptimize;

// Returns true if given function in lazily compiled.
var isLazy;

// Returns true if given function in interpreted.
var isInterpreted;

// Returns true if given function in baseline.
var isBaseline;

// Returns true if given function in unoptimized (interpreted or baseline).
var isUnoptimized;

// Returns true if given function is optimized.
var isOptimized;

// Returns true if given function will be compiled by Maglev.
var willBeMaglevved;

// Returns true if given function will be compiled by TurboFan.
var willBeTurbofanned;

// Returns true if given function is compiled by Maglev.
var isMaglevved;

// Returns true if given function is compiled by TurboFan.
var isTurboFanned;

// Returns true if the top frame in interpreted according to the status
// passed as a parameter.
var topFrameIsInterpreted;

// Returns true if the top frame in baseline according to the status
// passed as a parameter.
var topFrameIsBaseline;

// Returns true if the top frame in compiled by Maglev according to the
// status passed as a parameter.
var topFrameIsMaglevved;

// Returns true if the top frame in compiled by Turbofan according to the
// status passed as a parameter.
var topFrameIsTurboFanned;

// Monkey-patchable all-purpose failure handler.
var fail;

// Monkey-patchable all-purpose failure handler.
var failWithMessage;

// Returns the formatted failure text.  Used by test-async.js.
var formatFailureText;

// Returns a pretty-printed string representation of the passed value.
var prettyPrinted;

(function () {  // Scope for utility functions.

  var ObjectPrototypeToString = Object.prototype.toString;
  var NumberPrototypeValueOf = Number.prototype.valueOf;
  var BooleanPrototypeValueOf = Boolean.prototype.valueOf;
  var StringPrototypeValueOf = String.prototype.valueOf;
  var DatePrototypeValueOf = Date.prototype.valueOf;
  var RegExpPrototypeToString = RegExp.prototype.toString;
  var ArrayPrototypeForEach = Array.prototype.forEach;
  var ArrayPrototypeJoin = Array.prototype.join;
  var ArrayPrototypeMap = Array.prototype.map;
  var ArrayPrototypePush = Array.prototype.push;
  var JSONStringify = JSON.stringify;

  var BigIntPrototypeValueOf;
  // TODO(neis): Remove try-catch once BigInts are enabled by default.
  try {
    BigIntPrototypeValueOf = BigInt.prototype.valueOf;
  } catch (e) {}

  function classOf(object) {
    // Argument must not be null or undefined.
    var string = ObjectPrototypeToString.call(object);
    // String has format [object <ClassName>].
    return string.substring(8, string.length - 1);
  }


  function ValueOf(value) {
    switch (classOf(value)) {
      case "Number":
        return NumberPrototypeValueOf.call(value);
      case "BigInt":
        return BigIntPrototypeValueOf.call(value);
      case "String":
        return StringPrototypeValueOf.call(value);
      case "Boolean":
        return BooleanPrototypeValueOf.call(value);
      case "Date":
        return DatePrototypeValueOf.call(value);
      default:
        return value;
    }
  }


  prettyPrinted = function prettyPrinted(value) {
    let visited = new Set();
    function prettyPrint(value) {
      try {
        switch (typeof value) {
          case "string":
            return JSONStringify(value);
          case "bigint":
            return String(value) + "n";
          case "number":
            if (value === 0 && (1 / value) < 0) return "-0";
            // FALLTHROUGH.
          case "boolean":
          case "undefined":
          case "function":
          case "symbol":
            return String(value);
          case "object":
            if (value === null) return "null";
            // Guard against re-visiting.
            if (visited.has(value)) return "<...>";
            visited.add(value);
            var objectClass = classOf(value);
            switch (objectClass) {
              case "Number":
              case "BigInt":
              case "String":
              case "Boolean":
              case "Date":
                return objectClass + "(" + prettyPrint(ValueOf(value)) + ")";
              case "RegExp":
                return RegExpPrototypeToString.call(value);
              case "Array":
                var mapped = ArrayPrototypeMap.call(
                    value, (v,i,array)=>{
                      if (v === undefined && !(i in array)) return "";
                      return prettyPrint(v, visited);
                    });
                var joined = ArrayPrototypeJoin.call(mapped, ",");
                return "[" + joined + "]";
              case "Int8Array":
              case "Uint8Array":
              case "Uint8ClampedArray":
              case "Int16Array":
              case "Uint16Array":
              case "Int32Array":
              case "Uint32Array":
              case "Float32Array":
              case "Float64Array":
              case "BigInt64Array":
              case "BigUint64Array":
                var joined = ArrayPrototypeJoin.call(value, ",");
                return objectClass + "([" + joined + "])";
              case "Object":
                break;
              default:
                return objectClass + "(" + String(value) + ")";
            }
            // classOf() returned "Object".
            var name = value.constructor?.name ?? "Object";
            var pretty_properties = [];
            for (let [k,v] of Object.entries(value)) {
              ArrayPrototypePush.call(
                  pretty_properties, `${k}:${prettyPrint(v, visited)}`);
            }
            var joined = ArrayPrototypeJoin.call(pretty_properties, ",");
            return `${name}({${joined}})`;
          default:
            return "-- unknown value --";
        }
      } catch (e) {
        // Guard against general exceptions (especially stack overflows).
        return "<error>"
      }
    }
    return prettyPrint(value);
  }

  failWithMessage = function failWithMessage(message) {
    throw new MjsUnitAssertionError(()=>message);
  }

  formatFailureText = function(expectedText, found, name_opt) {
    var message = "Fail" + "ure";
    if (name_opt) {
      // Fix this when we ditch the old test runner.
      message += " (" + name_opt + ")";
    }

    var foundText = prettyPrinted(found);
    if (expectedText.length <= 40 && foundText.length <= 40) {
      message += ": expected <" + expectedText + "> found <" + foundText + ">";
    } else {
      message += ":\nexpected:\n" + expectedText + "\nfound:\n" + foundText;
    }
    return message;
  }

  fail = function fail(expectedText, found, name_opt) {
    throw new MjsUnitAssertionError(
        ()=>formatFailureText(expectedText, found, name_opt));
  }


  function deepObjectEquals(a, b) {
    // Note: This function does not check prototype equality.

    // For now, treat two objects the same even if some property is configured
    // differently (configurable, enumerable, writable).
    var aProps = Object.getOwnPropertyNames(a);
    aProps.sort();
    var bProps = Object.getOwnPropertyNames(b);
    bProps.sort();
    if (!deepEquals(aProps, bProps)) {
      return false;
    }
    for (var i = 0; i < aProps.length; i++) {
      if (!deepEquals(a[aProps[i]], b[aProps[i]])) {
        return false;
      }
    }
    return true;
  }

  deepEquals = function deepEquals(a, b) {
    if (a === b) {
      // Check for -0.
      if (a === 0) return (1 / a) === (1 / b);
      return true;
    }
    if (typeof a !== typeof b) return false;
    if (typeof a === 'number') return isNaN(a) && isNaN(b);
    if (typeof a !== 'object' && typeof a !== 'function') return false;
    // Neither a nor b is primitive.
    var objectClass = classOf(a);
    if (objectClass !== classOf(b)) return false;
    switch (objectClass) {
      case 'RegExp':
        // For RegExp, just compare pattern and flags using its toString.
        return RegExpPrototypeToString.call(a) ===
            RegExpPrototypeToString.call(b);
      case 'Function':
        // Functions are only identical to themselves.
        return false;
      case 'Array':
        if (a.length !== b.length) return false;
        for (var i = 0; i < a.length; i++) {
          if ((i in a) !== (i in b)) return false;
          if (!deepEquals(a[i], b[i])) return false;
        }
        return true;
      case 'Int8Array':
      case 'Uint8Array':
      case 'Uint8ClampedArray':
      case 'Int16Array':
      case 'Uint16Array':
      case 'Int32Array':
      case 'Uint32Array':
      case 'BigInt64Array':
      case 'BigUint64Array':
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) {
          if (a[i] !== b[i]) return false;
        }
        return true;
      case 'Float32Array':
      case 'Float64Array':
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) {
          if (!deepEquals(a[i], b[i])) return false;
        }
        return true;
      case 'String':
      case 'Number':
      case 'BigInt':
      case 'Boolean':
      case 'Date':
        return ValueOf(a) === ValueOf(b);
    }
    return deepObjectEquals(a, b);
  }

  assertSame = function assertSame(expected, found, name_opt) {
    if (Object.is(expected, found)) return;
    fail(prettyPrinted(expected), found, name_opt);
  };

  assertNotSame = function assertNotSame(expected, found, name_opt) {
    if (!Object.is(expected, found)) return;
    fail("not same as " + prettyPrinted(expected), found, name_opt);
  }

  assertEquals = function assertEquals(expected, found, name_opt) {
    if (!deepEquals(found, expected)) {
      fail(prettyPrinted(expected), found, name_opt);
    }
  };

  assertNotEquals = function assertNotEquals(expected, found, name_opt) {
    if (deepEquals(found, expected)) {
      fail("not equals to " + prettyPrinted(expected), found, name_opt);
    }
  };


  assertEqualsDelta =
      function assertEqualsDelta(expected, found, delta, name_opt) {
    if (Math.abs(expected - found) > delta) {
      fail(prettyPrinted(expected) + " +- " + prettyPrinted(delta), found, name_opt);
    }
  };


  assertArrayEquals = function assertArrayEquals(expected, found, name_opt) {
    var start = "";
    if (name_opt) {
      start = name_opt + " - ";
    }
    assertEquals(expected.length, found.length, start + "array length");
    if (expected.length === found.length) {
      for (var i = 0; i < expected.length; ++i) {
        assertEquals(expected[i], found[i],
                     start + "array element at index " + i);
      }
    }
  };


  assertPropertiesEqual = function assertPropertiesEqual(expected, found,
                                                         name_opt) {
    // Check properties only.
    if (!deepObjectEquals(expected, found)) {
      fail(expected, found, name_opt);
    }
  };


  assertToStringEquals = function assertToStringEquals(expected, found,
                                                       name_opt) {
    if (expected !== String(found)) {
      fail(expected, found, name_opt);
    }
  };


  assertTrue = function assertTrue(value, name_opt) {
    assertEquals(true, value, name_opt);
  };


  assertFalse = function assertFalse(value, name_opt) {
    assertEquals(false, value, name_opt);
  };


  assertNull = function assertNull(value, name_opt) {
    if (value !== null) {
      fail("null", value, name_opt);
    }
  };


  assertNotNull = function assertNotNull(value, name_opt) {
    if (value === null) {
      fail("not null", value, name_opt);
    }
  };

  function executeCode(code) {
    if (typeof code === 'function') return code();
    if (typeof code === 'string') return eval(code);
    failWithMessage(
        'Given code is neither function nor string, but ' + (typeof code) +
        ': <' + prettyPrinted(code) + '>');
  }

  assertException = function assertException(e, type_opt, cause_opt) {
    if (type_opt !== undefined) {
      assertEquals('function', typeof type_opt);
      assertInstanceof(e, type_opt);
    }
    if (RegExp !== undefined && cause_opt instanceof RegExp) {
      assertMatches(cause_opt, e.message, 'Error message');
    } else if (cause_opt !== undefined) {
      assertEquals(cause_opt, e.message, 'Error message');
    }
  }

  assertThrows = function assertThrows(code, type_opt, cause_opt) {
    if (arguments.length > 1 && type_opt === undefined) {
      failWithMessage('invalid use of assertThrows, unknown type_opt given');
    }
    if (type_opt !== undefined && typeof type_opt !== 'function') {
      failWithMessage(
          'invalid use of assertThrows, maybe you want assertThrowsEquals');
    }
    try {
      executeCode(code);
    } catch (e) {
      assertException(e, type_opt, cause_opt);
      return;
    }
    let msg = 'Did not throw exception';
    if (type_opt !== undefined && type_opt.name !== undefined)
      msg += ', expected ' + type_opt.name;
    failWithMessage(msg);
  };

  assertThrowsEquals = function assertThrowsEquals(fun, val) {
    try {
      fun();
    } catch (e) {
      assertSame(val, e);
      return;
    }
    failWithMessage('Did not throw exception, expected ' + prettyPrinted(val));
  };

  assertThrowsAsync = function assertThrowsAsync(promise, type_opt, cause_opt) {
    if (arguments.length > 1 && type_opt === undefined) {
      failWithMessage('invalid use of assertThrows, unknown type_opt given');
    }
    if (type_opt !== undefined && typeof type_opt !== 'function') {
      failWithMessage(
          'invalid use of assertThrows, maybe you want assertThrowsEquals');
    }
    let msg = 'Promise did not throw exception';
    if (type_opt !== undefined && type_opt.name !== undefined)
      msg += ', expected ' + type_opt.name;
    return assertPromiseResult(
        promise,
        // Use setTimeout to throw the error again to get out of the promise
        // chain.
        res => setTimeout(_ => fail('<throw>', res, msg), 0),
        e => assertException(e, type_opt, cause_opt));
  };

  assertEarlyError = function assertEarlyError(code) {
    try {
      new Function(code);
    } catch (e) {
      assertException(e, SyntaxError);
      return;
    }
    failWithMessage('Did not throw exception while parsing');
  }

  assertThrowsAtRuntime = function assertThrowsAtRuntime(code, type_opt) {
    const f = new Function(code);
    if (arguments.length > 1 && type_opt !== undefined) {
      assertThrows(f, type_opt);
    } else {
      assertThrows(f);
    }
  }

  assertInstanceof = function assertInstanceof(obj, type) {
    if (!(obj instanceof type)) {
      var actualTypeName = null;
      var actualConstructor = obj && Object.getPrototypeOf(obj).constructor;
      if (typeof actualConstructor === 'function') {
        actualTypeName = actualConstructor.name || String(actualConstructor);
      }
      failWithMessage(
          'Object <' + prettyPrinted(obj) + '> is not an instance of <' +
          (type.name || type) + '>' +
          (actualTypeName ? ' but of <' + actualTypeName + '>' : ''));
    }
  };

  assertDoesNotThrow = function assertDoesNotThrow(code, name_opt) {
    try {
      executeCode(code);
    } catch (e) {
      if (e instanceof MjsUnitAssertionError) throw e;
      failWithMessage("threw an exception: " + (e.message || e));
    }
  };

  assertUnreachable = function assertUnreachable(name_opt) {
    // Fix this when we ditch the old test runner.
    var message = "Fail" + "ure: unreachable";
    if (name_opt) {
      message += " - " + name_opt;
    }
    failWithMessage(message);
  };

  assertContains = function(sub, value, name_opt) {
    if (value == null ? (sub != null) : value.indexOf(sub) == -1) {
      fail("contains '" + String(sub) + "'", value, name_opt);
    }
  };

  assertMatches = function(regexp, str, name_opt) {
    if (!(regexp instanceof RegExp)) {
      regexp = new RegExp(regexp);
    }
    if (!str.match(regexp)) {
      fail("should match '" + regexp + "'", str, name_opt);
    }
  };

  function concatenateErrors(stack, exception) {
    // If the exception does not contain a stack trace, wrap it in a new Error.
    if (!exception.stack) exception = new Error(exception);

    // If the exception already provides a special stack trace, we do not modify
    // it.
    if (typeof exception.stack !== 'string') {
      return exception;
    }
    exception.stack = stack + '\n\n' + exception.stack;
    return exception;
  }

  assertPromiseResult = function(promise, success, fail) {
    if (success !== undefined) assertEquals('function', typeof success);
    if (fail !== undefined) assertEquals('function', typeof fail);
    assertInstanceof(promise, Promise);
    const stack = (new Error()).stack;

    var test_promise = promise.then(
        result => {
          try {
            if (success !== undefined) success(result);
          } catch (e) {
            // Use setTimeout to throw the error again to get out of the promise
            // chain.
            setTimeout(_ => {
              throw concatenateErrors(stack, e);
            }, 0);
          }
        },
        result => {
          try {
            if (fail === undefined) throw result;
            fail(result);
          } catch (e) {
            // Use setTimeout to throw the error again to get out of the promise
            // chain.
            setTimeout(_ => {
              throw concatenateErrors(stack, e);
            }, 0);
          }
        });

    if (!promiseTestChain) promiseTestChain = Promise.resolve();
    return promiseTestChain.then(test_promise);
  };

  var OptimizationStatusImpl = undefined;

  var OptimizationStatus = function(fun) {
    if (OptimizationStatusImpl === undefined) {
      try {
        OptimizationStatusImpl = new Function(
            "fun", "return %GetOptimizationStatus(fun);");
      } catch (e) {
        throw new Error("natives syntax not allowed");
      }
    }
    return OptimizationStatusImpl(fun);
  }

  assertUnoptimized = function assertUnoptimized(
      fun, name_opt, skip_if_maybe_deopted = true) {
    var opt_status = OptimizationStatus(fun);
    name_opt = name_opt ?? fun.name;
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0, name_opt);
    if (skip_if_maybe_deopted &&
        (opt_status & V8OptimizationStatus.kMaybeDeopted) !== 0) {
      // When --deopt-every-n-times flag is specified it's no longer guaranteed
      // that particular function is still deoptimized, so keep running the test
      // to stress test the deoptimizer.
      return;
    }
    var is_optimized = (opt_status & V8OptimizationStatus.kOptimized) !== 0;
    if (is_optimized && (opt_status & V8OptimizationStatus.kMaglevved) &&
        (opt_status &
         V8OptimizationStatus.kOptimizeOnNextCallOptimizesToMaglev)) {
      // When --optimize-on-next-call-optimizes-to-maglev is used, we might emit
      // more generic code than optimization tests expect. In such cases,
      // assertUnoptimized may see optimized code, but we still want it to
      // succeed and continue the test.
      return;
    }
    if (is_optimized && (opt_status & V8OptimizationStatus.kTurboFanned) &&
        (opt_status &
         V8OptimizationStatus.kOptimizeMaglevOptimizesToTurbofan)) {
      // In some cases, Turbofan actually emits more generic code than Maglev
      // (for instance, allowing Oddballs where Maglev only allows HeapNumbers),
      // and assertUnoptimized with --optimize-maglev-optimizes-to-turbofan will
      // fail. In those cases, we still want this assert to succeed and the test
      // to continue.
      return;
    }
    assertFalse(is_optimized, 'should not be optimized: ' + name_opt);
  }

  assertOptimized = function assertOptimized(
      fun, name_opt, skip_if_maybe_deopted = true) {
    var opt_status = OptimizationStatus(fun);
    name_opt = name_opt ?? fun.name;
    // Tests that use assertOptimized() do not make sense for Lite mode where
    // optimization is always disabled, explicitly exit the test with a warning.
    if (opt_status & V8OptimizationStatus.kLiteMode) {
      print("Warning: Test uses assertOptimized in Lite mode, skipping test.");
      quit(0);
    }
    // Tests that use assertOptimized() do not make sense if --no-turbofan
    // option is provided. Such tests must add --turbofan to flags comment.
    assertFalse((opt_status & V8OptimizationStatus.kNeverOptimize) !== 0,
                "test does not make sense with --no-turbofan");
    assertTrue(
        (opt_status & V8OptimizationStatus.kIsFunction) !== 0,
        'should be a function: ' + name_opt);
    if (skip_if_maybe_deopted &&
        (opt_status & V8OptimizationStatus.kMaybeDeopted) !== 0) {
      // When --deopt-every-n-times flag is specified it's no longer guaranteed
      // that particular function is still optimized, so keep running the test
      // to stress test the deoptimizer.
      return;
    }
    if ((opt_status &
         V8OptimizationStatus.kOptimizeMaglevOptimizesToTurbofan) !== 0) {
      // When --optimize-maglev-optimizes-to-turbofan is used it's no longer
      // guaranteed that a particular function stays optimized the same way
      // as with Maglev.
      return;
    }
    assertTrue(
        (opt_status & V8OptimizationStatus.kOptimized) !== 0,
        'should be optimized: ' + name_opt);
  }

  isNeverOptimizeLiteMode = function isNeverOptimizeLiteMode() {
    var opt_status = OptimizationStatus(undefined, "");
    return (opt_status & V8OptimizationStatus.kLiteMode) !== 0;
  }

  isNeverOptimize = function isNeverOptimize() {
    var opt_status = OptimizationStatus(undefined, "");
    return (opt_status & V8OptimizationStatus.kNeverOptimize) !== 0;
  }

  isLazy = function isLazy(fun) {
    var opt_status = OptimizationStatus(fun, '');
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");
    return (opt_status & V8OptimizationStatus.kIsLazy) !== 0;
  }

  isInterpreted = function isInterpreted(fun) {
    var opt_status = OptimizationStatus(fun, "");
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");
    return (opt_status & V8OptimizationStatus.kOptimized) === 0 &&
           (opt_status & V8OptimizationStatus.kInterpreted) !== 0;
  }

  isBaseline = function isBaseline(fun) {
    var opt_status = OptimizationStatus(fun, "");
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");
    return (opt_status & V8OptimizationStatus.kOptimized) === 0 &&
           (opt_status & V8OptimizationStatus.kBaseline) !== 0;
  }

  isUnoptimized = function isUnoptimized(fun) {
    return isInterpreted(fun) || isBaseline(fun);
  }

  isOptimized = function isOptimized(fun) {
    var opt_status = OptimizationStatus(fun, "");
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");
    return (opt_status & V8OptimizationStatus.kOptimized) !== 0;
  }

  isMaglevved = function isMaglevved(fun) {
    var opt_status = OptimizationStatus(fun, "");
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");

    const is_optimized = (opt_status & V8OptimizationStatus.kOptimized) !== 0;
    const is_maglevved = (opt_status & V8OptimizationStatus.kMaglevved) !== 0;
    // When --optimize-maglev-optimizes-to-turbofan is used, in many tests this
    // method is synonym with isTurboFanned. Tests where this doesn't hold
    // might need to negate that flag.
    const is_turbofanned_by_flag = (
        (opt_status & V8OptimizationStatus.kTurboFanned) !== 0 &&
        (opt_status & V8OptimizationStatus.kOptimizeMaglevOptimizesToTurbofan) !== 0);
    return is_optimized && (is_maglevved || is_turbofanned_by_flag);
  }

  willBeMaglevved = function willBeMaglevved(fun) {
    var opt_status = OptimizationStatus(fun, "");
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");
    return (opt_status & V8OptimizationStatus.kOptimizeOnNextCallOptimizesToMaglev) !== 0;
  }

  willBeTurbofanned = function willBeTurbofanned(fun) {
    var opt_status = OptimizationStatus(fun, "");
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");
    return (opt_status & V8OptimizationStatus.kOptimizeOnNextCallOptimizesToMaglev) === 0;
  }

  isTurboFanned = function isTurboFanned(fun) {
    var opt_status = OptimizationStatus(fun, "");
    assertTrue((opt_status & V8OptimizationStatus.kIsFunction) !== 0,
               "not a function");
    return (opt_status & V8OptimizationStatus.kOptimized) !== 0 &&
           (opt_status & V8OptimizationStatus.kTurboFanned) !== 0;
  }

  topFrameIsInterpreted = function topFrameIsInterpreted(opt_status) {
    assertNotEquals(opt_status, undefined);
    return (opt_status & V8OptimizationStatus.kTopmostFrameIsInterpreted) !== 0;
  }

  topFrameIsBaseline = function topFrameIsBaseline(opt_status) {
    assertNotEquals(opt_status, undefined);
    return (opt_status & V8OptimizationStatus.kTopmostFrameIsBaseline) !== 0;
  }

  topFrameIsMaglevved = function topFrameIsMaglevved(opt_status) {
    assertNotEquals(opt_status, undefined);
    return (opt_status & V8OptimizationStatus.kTopmostFrameIsMaglev) !== 0;
  }

  topFrameIsTurboFanned = function topFrameIsTurboFanned(opt_status) {
    assertNotEquals(opt_status, undefined);
    return (opt_status & V8OptimizationStatus.kTopmostFrameIsTurboFanned) !== 0;
  }

  // Custom V8-specific stack trace formatter that is temporarily installed on
  // the Error object.
  MjsUnitAssertionError.prepareStackTrace = function(error, stack) {
    // Trigger default formatting with recursion.
    try {
      // Filter-out all but the first mjsunit frame.
      let filteredStack = [];
      let inMjsunit = true;
      for (let i = 0; i < stack.length; i++) {
        let frame = stack[i];
        if (inMjsunit) {
          let file = frame.getFileName();
          if (!file || !file.endsWith("mjsunit.js")) {
            inMjsunit = false;
            // Push the last mjsunit frame, typically containing the assertion
            // function.
            if (i > 0) ArrayPrototypePush.call(filteredStack, stack[i-1]);
            ArrayPrototypePush.call(filteredStack, stack[i]);
          }
          continue;
        }
        ArrayPrototypePush.call(filteredStack, frame);
      }
      stack = filteredStack;

      // Infer function names and calculate {max_name_length}
      let max_name_length = 0;
      ArrayPrototypeForEach.call(stack, each => {
        let name = each.getFunctionName();
        if (name == null) name = "";
        if (each.isEval()) {
          name = name;
        } else if (each.isConstructor()) {
          name = "new " + name;
        } else if (each.isNative()) {
          name = "native " + name;
        } else if (!each.isToplevel()) {
          name = each.getTypeName() + "." + name;
        }
        each.name = name;
        max_name_length = Math.max(name.length, max_name_length)
      });

      // Format stack frames.
      stack = ArrayPrototypeMap.call(stack, each => {
        let frame = "    at " + each.name.padEnd(max_name_length);
        let fileName = each.getFileName();
        if (each.isEval()) return frame + " " + each.getEvalOrigin();
        frame += " " + (fileName ? fileName : "");
        let line= each.getLineNumber();
        frame += " " + (line ? line : "");
        let column = each.getColumnNumber();
        frame += (column ? ":" + column : "");
        return frame;
      });
      return "" + error.message + "\n" + ArrayPrototypeJoin.call(stack, "\n");
    } catch (e) {};
    return error.stack;
  }
})();

// ==== harmony/regexp-named-captures.js ====
// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax

// Malformed named captures.
assertThrows("/(?<>a)/u", SyntaxError);  // Empty name.
assertThrows("/(?<aa)/u", SyntaxError);  // Unterminated name.
assertThrows("/(?<42a>a)/u", SyntaxError);  // Name starting with digits.
assertThrows("/(?<:a>a)/u", SyntaxError);  // Name starting with invalid char.
assertThrows("/(?<a:>a)/u", SyntaxError);  // Name containing with invalid char.
assertThrows("/(?<a>a)(?<a>a)/u", SyntaxError);  // Duplicate name.
assertThrows("/(?<a>a)(?<b>b)(?<a>a)/u", SyntaxError);  // Duplicate name.
assertThrows("/\\k<a>/u", SyntaxError);  // Invalid reference.
assertThrows("/\\k<a/u", SyntaxError);  // Unterminated reference.
assertThrows("/\\k/u", SyntaxError);  // Lone \k.
assertThrows("/(?<a>.)\\k/u", SyntaxError);  // Lone \k.
assertThrows("/(?<a>.)\\k<a/u", SyntaxError);  // Unterminated reference.
assertThrows("/(?<a>.)\\k<b>/u", SyntaxError);  // Invalid reference.
assertThrows("/(?<a>a)\\k<ab>/u", SyntaxError);  // Invalid reference.
assertThrows("/(?<ab>a)\\k<a>/u", SyntaxError);  // Invalid reference.
assertThrows("/\\k<a>(?<ab>a)/u", SyntaxError);  // Invalid reference.
assertThrows("/(?<a>\\a)/u", SyntaxError);  // Identity escape in capture.

// Behavior in non-unicode mode.
assertThrows("/(?<>a)/", SyntaxError);
assertThrows("/(?<aa)/", SyntaxError);
assertThrows("/(?<42a>a)/", SyntaxError);
assertThrows("/(?<:a>a)/", SyntaxError);
assertThrows("/(?<a:>a)/", SyntaxError);
assertThrows("/(?<a>a)(?<a>a)/", SyntaxError);
assertThrows("/(?<a>a)(?<b>b)(?<a>a)/", SyntaxError);
assertTrue(/\k<a>/.test("k<a>"));
assertTrue(/\k<4>/.test("k<4>"));
assertTrue(/\k<a/.test("k<a"));
assertTrue(/\k/.test("k"));
assertThrows("/(?<a>.)\\k/", SyntaxError);
assertThrows("/(?<a>.)\\k<a/", SyntaxError);
assertThrows("/(?<a>.)\\k<b>/", SyntaxError);
assertThrows("/(?<a>a)\\k<ab>/", SyntaxError);
assertThrows("/(?<ab>a)\\k<a>/", SyntaxError);
assertThrows("/\\k<a>(?<ab>a)/", SyntaxError);
assertThrows("/\\k<a(?<a>a)/", SyntaxError);
assertTrue(/(?<a>\a)/.test("a"));

assertEquals(["k<a>"], "xxxk<a>xxx".match(/\k<a>/));
assertEquals(["k<a"], "xxxk<a>xxx".match(/\k<a/));

assertEquals({a: "a", b: "b", c: "c"},
             /(?<a>.)(?<b>.)(?<c>.)\k<c>\k<b>\k<a>/.exec("abccba").groups);

// A couple of corner cases around '\k' as named back-references vs. identity
// escapes.
assertTrue(/\k<a>(?<=>)a/.test("k<a>a"));
assertTrue(/\k<a>(?<!a)a/.test("k<a>a"));
assertTrue(/\k<a>(<a>x)/.test("k<a><a>x"));
assertTrue(/\k<a>(?<a>x)/.test("x"));
assertThrows("/\\k<a>(?<b>x)/", SyntaxError);
assertThrows("/\\k<a(?<a>.)/", SyntaxError);
assertThrows("/\\k(?<a>.)/", SyntaxError);

// Basic named groups.
assertEquals(["a", "a"], "bab".match(/(?<a>a)/u));
assertEquals(["a", "a"], "bab".match(/(?<a42>a)/u));
assertEquals(["a", "a"], "bab".match(/(?<_>a)/u));
assertEquals(["a", "a"], "bab".match(/(?<$>a)/u));
assertEquals(["bab", "a"], "bab".match(/.(?<$>a)./u));
assertEquals(["bab", "a", "b"], "bab".match(/.(?<a>a)(.)/u));
assertEquals(["bab", "a", "b"], "bab".match(/.(?<a>a)(?<b>.)/u));
assertEquals(["bab", "ab"], "bab".match(/.(?<a>\w\w)/u));
assertEquals(["bab", "bab"], "bab".match(/(?<a>\w\w\w)/u));
assertEquals(["bab", "ba", "b"], "bab".match(/(?<a>\w\w)(?<b>\w)/u));

assertEquals(["a", "a"], "bab".match(/(?<a>a)/));
assertEquals(["a", "a"], "bab".match(/(?<a42>a)/));
assertEquals(["a", "a"], "bab".match(/(?<_>a)/));
assertEquals(["a", "a"], "bab".match(/(?<$>a)/));
assertEquals(["bab", "a"], "bab".match(/.(?<$>a)./));
assertEquals(["bab", "a", "b"], "bab".match(/.(?<a>a)(.)/));
assertEquals(["bab", "a", "b"], "bab".match(/.(?<a>a)(?<b>.)/));
assertEquals(["bab", "ab"], "bab".match(/.(?<a>\w\w)/));
assertEquals(["bab", "bab"], "bab".match(/(?<a>\w\w\w)/));
assertEquals(["bab", "ba", "b"], "bab".match(/(?<a>\w\w)(?<b>\w)/));

assertEquals("bab".match(/(a)/u), "bab".match(/(?<a>a)/u));
assertEquals("bab".match(/(a)/u), "bab".match(/(?<a42>a)/u));
assertEquals("bab".match(/(a)/u), "bab".match(/(?<_>a)/u));
assertEquals("bab".match(/(a)/u), "bab".match(/(?<$>a)/u));
assertEquals("bab".match(/.(a)./u), "bab".match(/.(?<$>a)./u));
assertEquals("bab".match(/.(a)(.)/u), "bab".match(/.(?<a>a)(.)/u));
assertEquals("bab".match(/.(a)(.)/u), "bab".match(/.(?<a>a)(?<b>.)/u));
assertEquals("bab".match(/.(\w\w)/u), "bab".match(/.(?<a>\w\w)/u));
assertEquals("bab".match(/(\w\w\w)/u), "bab".match(/(?<a>\w\w\w)/u));
assertEquals("bab".match(/(\w\w)(\w)/u), "bab".match(/(?<a>\w\w)(?<b>\w)/u));

assertEquals(["bab", "b"], "bab".match(/(?<b>b).\1/u));
assertEquals(["baba", "b", "a"], "baba".match(/(.)(?<a>a)\1\2/u));
assertEquals(["baba", "b", "a", "b", "a"],
    "baba".match(/(.)(?<a>a)(?<b>\1)(\2)/u));
assertEquals(["<a", "<"], "<a".match(/(?<lt><)a/u));
assertEquals([">a", ">"], ">a".match(/(?<gt>>)a/u));

// Named references.
assertEquals(["bab", "b"], "bab".match(/(?<b>.).\k<b>/u));
assertNull("baa".match(/(?<b>.).\k<b>/u));

// Nested groups.
assertEquals(["bab", "bab", "ab", "b"], "bab".match(/(?<a>.(?<b>.(?<c>.)))/u));
assertEquals({a: "bab", b: "ab", c: "b"},
             "bab".match(/(?<a>.(?<b>.(?<c>.)))/u).groups);

// Reference inside group.
assertEquals(["bab", "b"], "bab".match(/(?<a>\k<a>\w)../u));
assertEquals({a: "b"}, "bab".match(/(?<a>\k<a>\w)../u).groups);

// Reference before group.
assertEquals(["bab", "b"], "bab".match(/\k<a>(?<a>b)\w\k<a>/u));
assertEquals({a: "b"}, "bab".match(/\k<a>(?<a>b)\w\k<a>/u).groups);
assertEquals(["bab", "b", "a"], "bab".match(/(?<b>b)\k<a>(?<a>a)\k<b>/u));
assertEquals({a: "a", b: "b"},
             "bab".match(/(?<b>b)\k<a>(?<a>a)\k<b>/u).groups);

assertEquals(["bab", "b"], "bab".match(/\k<a>(?<a>b)\w\k<a>/));
assertEquals(["bab", "b", "a"], "bab".match(/(?<b>b)\k<a>(?<a>a)\k<b>/));

// Reference properties.
assertEquals("a", /(?<a>a)(?<b>b)\k<a>/u.exec("aba").groups.a);
assertEquals("b", /(?<a>a)(?<b>b)\k<a>/u.exec("aba").groups.b);
assertEquals(undefined, /(?<a>a)(?<b>b)\k<a>/u.exec("aba").groups.c);
assertEquals(undefined, /(?<a>a)(?<b>b)\k<a>|(?<c>c)/u.exec("aba").groups.c);

// Unicode names.
assertEquals("a", /(?<π>a)/u.exec("bab").groups.π);
assertEquals("a", /(?<\u{03C0}>a)/u.exec("bab").groups.π);
assertEquals("a", /(?<π>a)/u.exec("bab").groups.\u03C0);
assertEquals("a", /(?<\u{03C0}>a)/u.exec("bab").groups.\u03C0);
assertEquals("a", /(?<$>a)/u.exec("bab").groups.$);
assertEquals("a", /(?<_>a)/u.exec("bab").groups._);
assertEquals("a", /(?<$𐒤>a)/u.exec("bab").groups.$𐒤);
assertEquals("a", /(?<_\u200C>a)/u.exec("bab").groups._\u200C);
assertEquals("a", /(?<_\u200D>a)/u.exec("bab").groups._\u200D);
assertEquals("a", /(?<ಠ_ಠ>a)/u.exec("bab").groups.ಠ_ಠ);
assertThrows('/(?<❤>a)/u', SyntaxError);
assertThrows('/(?<𐒤>a)/u', SyntaxError);  // ID_Continue but not ID_Start.

assertEquals("a", /(?<π>a)/.exec("bab").groups.π);
assertEquals("a", /(?<$>a)/.exec("bab").groups.$);
assertEquals("a", /(?<_>a)/.exec("bab").groups._);
assertEquals("a", /(?<$𐒤>a)/.exec("bab").groups.$𐒤);
assertEquals("a", /(?<ಠ_ಠ>a)/.exec("bab").groups.ಠ_ಠ);
assertThrows('/(?<❤>a)/', SyntaxError);
assertThrows('/(?<𐒤>a)/', SyntaxError);  // ID_Continue but not ID_Start.

// Interaction with lookbehind assertions.
assertEquals(["f", "c"], "abcdef".match(/(?<=(?<a>\w){3})f/u));
assertEquals({a: "c"}, "abcdef".match(/(?<=(?<a>\w){3})f/u).groups);
assertEquals({a: "b"}, "abcdef".match(/(?<=(?<a>\w){4})f/u).groups);
assertEquals({a: "a"}, "abcdef".match(/(?<=(?<a>\w)+)f/u).groups);
assertNull("abcdef".match(/(?<=(?<a>\w){6})f/u));

assertEquals(["f", ""], "abcdef".match(/((?<=\w{3}))f/u));
assertEquals(["f", ""], "abcdef".match(/(?<a>(?<=\w{3}))f/u));

assertEquals(["f", undefined], "abcdef".match(/(?<!(?<a>\d){3})f/u));
assertNull("abcdef".match(/(?<!(?<a>\D){3})f/u));

assertEquals(["f", undefined], "abcdef".match(/(?<!(?<a>\D){3})f|f/u));
assertEquals(["f", undefined], "abcdef".match(/(?<a>(?<!\D{3}))f|f/u));

// Properties created on result.groups.
assertEquals(["fst", "snd"],
             Object.getOwnPropertyNames(
                 /(?<fst>.)|(?<snd>.)/u.exec("abcd").groups));

// The '__proto__' property on the groups object.
assertEquals(undefined, /(?<a>.)/u.exec("a").groups.__proto__);
assertEquals("a", /(?<__proto__>a)/u.exec("a").groups.__proto__);

// Backslash as ID_Start and ID_Continue (v8:5868).
assertThrows("/(?<\\>.)/", SyntaxError);   // '\' misclassified as ID_Start.
assertThrows("/(?<a\\>.)/", SyntaxError);  // '\' misclassified as ID_Continue.

// Backreference before the group (exercises the capture mini-parser).
assertThrows("/\\1(?:.)/u", SyntaxError);
assertThrows("/\\1(?<=a)./u", SyntaxError);
assertThrows("/\\1(?<!a)./u", SyntaxError);
assertEquals(["a", "a"], /\1(?<a>.)/u.exec("abcd"));

// Unicode escapes in capture names.
assertTrue(/(?<a\uD801\uDCA4>.)/u.test("a"));  // \u Lead \u Trail
assertThrows("/(?<a\\uD801>.)/u", SyntaxError);  // \u Lead
assertThrows("/(?<a\\uDCA4>.)/u", SyntaxError);  // \u Trail
assertTrue(/(?<\u0041>.)/u.test("a"));  // \u NonSurrogate
assertTrue(/(?<\u{0041}>.)/u.test("a"));  // \u{ Non-surrogate }
assertTrue(/(?<a\u{104A4}>.)/u.test("a"));  // \u{ Surrogate, ID_Continue }
assertThrows("/(?<a\\u{110000}>.)/u", SyntaxError);  // \u{ Out-of-bounds }
assertThrows("/(?<a\\uD801>.)/u", SyntaxError);  // Lead
assertThrows("/(?<a\\uDCA4>.)/u", SyntaxError);  // Trail
assertThrows("/(?<a\uD801>.)/u", SyntaxError);  // Lead
assertThrows("/(?<a\uDCA4>.)/u", SyntaxError);  // Trail
assertTrue(RegExp("(?<\\u{0041}>.)", "u").test("a"));  // Non-surrogate
assertTrue(RegExp("(?<a\\u{104A4}>.)", "u").test("a"));  // Surrogate,ID_Continue
assertTrue(RegExp("(?<\u{0041}>.)", "u").test("a"));  // Non-surrogate
assertTrue(RegExp("(?<a\u{104A4}>.)", "u").test("a"));  // Surrogate,ID_Continue
assertTrue(RegExp("(?<\\u0041>.)", "u").test("a"));  // Non-surrogate

assertThrows("/(?<a\\uD801\uDCA4>.)/", SyntaxError);
assertThrows("/(?<a\\uD801>.)/", SyntaxError);
assertThrows("/(?<a\\uDCA4>.)/", SyntaxError);
assertTrue(/(?<\u0041>.)/.test("a"));
assertTrue(/(?<\u{0041}>.)/.test("a"));
assertTrue(/(?<a\u{104A4}>.)/.test("a"));
assertThrows("/(?<a\\u{10FFFF}>.)/", SyntaxError);
assertThrows("/(?<a\\uD801>.)/", SyntaxError);     // Lead
assertThrows("/(?<a\\uDCA4>.)/", SyntaxError);     // Trail
assertThrows("/(?<a\uD801>.)/", SyntaxError);      // Lead
assertThrows("/(?<a\uDCA4>.)/", SyntaxError);      // Trail
assertTrue(/(?<\u{0041}>.)/.test("a"));            // Non-surrogate
assertTrue(/(?<a\u{104A4}>.)/.test("a"));          // Surrogate, ID_Continue
assertTrue(RegExp("(?<\u{0041}>.)").test("a"));    // Non-surrogate
assertTrue(RegExp("(?<a\u{104A4}>.)").test("a"));  // Surrogate, ID_Continue
assertTrue(RegExp("(?<\\u0041>.)").test("a"));     // Non-surrogate

// @@replace with a callable replacement argument (no named captures).
{
  let result = "abcd".replace(/(.)(.)/u, (match, fst, snd, offset, str) => {
    assertEquals("ab", match);
    assertEquals("a", fst);
    assertEquals("b", snd);
    assertEquals(0, offset);
    assertEquals("abcd", str);
    return `${snd}${fst}`;
  });
  assertEquals("bacd", result);

  assertEquals("undefinedbcd", "abcd".replace(/(.)|(.)/u,
      (match, fst, snd, offset, str) => snd));
}

// @@replace with a callable replacement argument (global, named captures).
{
  let i = 0;
  let result = "abcd".replace(/(?<fst>.)(?<snd>.)/gu,
      (match, fst, snd, offset, str, groups) => {
    if (i == 0) {
      assertEquals("ab", match);
      assertEquals("a", groups.fst);
      assertEquals("b", groups.snd);
      assertEquals("a", fst);
      assertEquals("b", snd);
      assertEquals(0, offset);
      assertEquals("abcd", str);
    } else if (i == 1) {
      assertEquals("cd", match);
      assertEquals("c", groups.fst);
      assertEquals("d", groups.snd);
      assertEquals("c", fst);
      assertEquals("d", snd);
      assertEquals(2, offset);
      assertEquals("abcd", str);
    } else {
      assertUnreachable();
    }
    i++;
    return `${groups.snd}${groups.fst}`;
  });
  assertEquals("badc", result);

  assertEquals("undefinedundefinedundefinedundefined",
      "abcd".replace(/(?<fst>.)|(?<snd>.)/gu,
            (match, fst, snd, offset, str, groups) => groups.snd));
}

// @@replace with a callable replacement argument (non-global, named captures).
{
  let result = "abcd".replace(/(?<fst>.)(?<snd>.)/u,
      (match, fst, snd, offset, str, groups) => {
    assertEquals("ab", match);
    assertEquals("a", groups.fst);
    assertEquals("b", groups.snd);
    assertEquals("a", fst);
    assertEquals("b", snd);
    assertEquals(0, offset);
    assertEquals("abcd", str);
    return `${groups.snd}${groups.fst}`;
  });
  assertEquals("bacd", result);

  assertEquals("undefinedbcd",
      "abcd".replace(/(?<fst>.)|(?<snd>.)/u,
            (match, fst, snd, offset, str, groups) => groups.snd));
}

function toSlowMode(re) {
  re.exec = (str) => RegExp.prototype.exec.call(re, str);
  return re;
}

// @@replace with a callable replacement argument (slow, global,
// named captures).
{
  let i = 0;
  let re = toSlowMode(/(?<fst>.)(?<snd>.)/gu);
  let result = "abcd".replace(re, (match, fst, snd, offset, str, groups) => {
    if (i == 0) {
      assertEquals("ab", match);
      assertEquals("a", groups.fst);
      assertEquals("b", groups.snd);
      assertEquals("a", fst);
      assertEquals("b", snd);
      assertEquals(0, offset);
      assertEquals("abcd", str);
    } else if (i == 1) {
      assertEquals("cd", match);
      assertEquals("c", groups.fst);
      assertEquals("d", groups.snd);
      assertEquals("c", fst);
      assertEquals("d", snd);
      assertEquals(2, offset);
      assertEquals("abcd", str);
    } else {
      assertUnreachable();
    }
    i++;
    return `${groups.snd}${groups.fst}`;
  });
  assertEquals("badc", result);

  assertEquals("undefinedundefinedundefinedundefined",
      "abcd".replace(toSlowMode(/(?<fst>.)|(?<snd>.)/gu),
            (match, fst, snd, offset, str, groups) => groups.snd));
}

// @@replace with a callable replacement argument (slow, non-global,
// named captures).
{
  let re = toSlowMode(/(?<fst>.)(?<snd>.)/u);
  let result = "abcd".replace(re, (match, fst, snd, offset, str, groups) => {
    assertEquals("ab", match);
    assertEquals("a", groups.fst);
    assertEquals("b", groups.snd);
    assertEquals("a", fst);
    assertEquals("b", snd);
    assertEquals(0, offset);
    assertEquals("abcd", str);
    return `${groups.snd}${groups.fst}`;
  });
  assertEquals("bacd", result);

  assertEquals("undefinedbcd",
      "abcd".replace(toSlowMode(/(?<fst>.)|(?<snd>.)/u),
            (match, fst, snd, offset, str, groups) => groups.snd));
}

// @@replace with a string replacement argument (no named captures).
{
  let re = /(.)(.)|(x)/u;
  assertEquals("$<snd>$<fst>cd", "abcd".replace(re, "$<snd>$<fst>"));
  assertEquals("bacd", "abcd".replace(re, "$2$1"));
  assertEquals("cd", "abcd".replace(re, "$3"));
  assertEquals("$<sndcd", "abcd".replace(re, "$<snd"));
  assertEquals("$<sndacd", "abcd".replace(re, "$<snd$1"));
  assertEquals("$<42a>cd", "abcd".replace(re, "$<42$1>"));
  assertEquals("$<fth>cd", "abcd".replace(re, "$<fth>"));
  assertEquals("$<a>cd", "abcd".replace(re, "$<$1>"));
}

// @@replace with a string replacement argument (global, named captures).
{
  let re = /(?<fst>.)(?<snd>.)|(?<thd>x)/gu;
  assertEquals("badc", "abcd".replace(re, "$<snd>$<fst>"));
  assertEquals("badc", "abcd".replace(re, "$2$1"));
  assertEquals("", "abcd".replace(re, "$<thd>"));
  assertEquals("$<snd$<snd", "abcd".replace(re, "$<snd"));
  assertEquals("$<snda$<sndc", "abcd".replace(re, "$<snd$1"));
  assertEquals("", "abcd".replace(re, "$<42$1>"));
  assertEquals("", "abcd".replace(re, "$<fth>"));
  assertEquals("", "abcd".replace(re, "$<$1>"));
}

// @@replace with a string replacement argument (non-global, named captures).
{
  let re = /(?<fst>.)(?<snd>.)|(?<thd>x)/u;
  assertEquals("bacd", "abcd".replace(re, "$<snd>$<fst>"));
  assertEquals("bacd", "abcd".replace(re, "$2$1"));
  assertEquals("cd", "abcd".replace(re, "$<thd>"));
  assertEquals("$<sndcd", "abcd".replace(re, "$<snd"));
  assertEquals("$<sndacd", "abcd".replace(re, "$<snd$1"));
  assertEquals("cd", "abcd".replace(re, "$<42$1>"));
  assertEquals("cd", "abcd".replace(re, "$<fth>"));
  assertEquals("cd", "abcd".replace(re, "$<$1>"));
}

// @@replace with a string replacement argument (slow, global, named captures).
{
  let re = toSlowMode(/(?<fst>.)(?<snd>.)|(?<thd>x)/gu);
  assertEquals("badc", "abcd".replace(re, "$<snd>$<fst>"));
  assertEquals("badc", "abcd".replace(re, "$2$1"));
  assertEquals("", "abcd".replace(re, "$<thd>"));
  assertEquals("$<snd$<snd", "abcd".replace(re, "$<snd"));
  assertEquals("$<snda$<sndc", "abcd".replace(re, "$<snd$1"));
  assertEquals("", "abcd".replace(re, "$<42$1>"));
  assertEquals("", "abcd".replace(re, "$<fth>"));
  assertEquals("", "abcd".replace(re, "$<$1>"));
}

// @@replace with a string replacement argument (slow, non-global,
// named captures).
{
  let re = toSlowMode(/(?<fst>.)(?<snd>.)|(?<thd>x)/u);
  assertEquals("bacd", "abcd".replace(re, "$<snd>$<fst>"));
  assertEquals("bacd", "abcd".replace(re, "$2$1"));
  assertEquals("cd", "abcd".replace(re, "$<thd>"));
  assertEquals("$<sndcd", "abcd".replace(re, "$<snd"));
  assertEquals("$<sndacd", "abcd".replace(re, "$<snd$1"));
  assertEquals("cd", "abcd".replace(re, "$<42$1>"));
  assertEquals("cd", "abcd".replace(re, "$<fth>"));
  assertEquals("cd", "abcd".replace(re, "$<$1>"));
}

// Named captures are ordered by capture index on the groups object.
// https://crbug.com/v8/9822

{
  const r = /(?<BKey>.+)\s(?<AKey>.+)/;
  const s = 'example string';
  assertArrayEquals(["BKey", "AKey"], Object.keys(r.exec(s).groups));
}

// Tests for 'groups' semantics on the regexp result object.
// https://crbug.com/v8/7192

{
  const re = /./;
  const result = re.exec("a");
  assertTrue(true);
  assertEquals(result.__proto__, Array.prototype);
  assertTrue(result.hasOwnProperty('groups'));
  assertArrayEquals(["a"], result);
  assertEquals(0, result.index);
  assertEquals(undefined, result.groups);

  Array.prototype.groups = { a: "b" };
  assertTrue(true);
  assertEquals("$<a>", "a".replace(re, "$<a>"));
  Array.prototype.groups = undefined;
}

{
  const re = toSlowMode(/./);
  const result = re.exec("a");
  assertTrue(true);
  assertEquals(result.__proto__, Array.prototype);
  assertTrue(result.hasOwnProperty('groups'));
  assertArrayEquals(["a"], result);
  assertEquals(0, result.index);
  assertEquals(undefined, result.groups);

  Array.prototype.groups = { a: "b" };
  assertTrue(true);
  assertEquals("$<a>", "a".replace(re, "$<a>"));
  Array.prototype.groups = undefined;
}

{
  const re = /(?<a>a).|(?<x>x)/;
  const result = re.exec("ab");
  assertTrue(true);
  assertEquals(result.__proto__, Array.prototype);
  assertTrue(result.hasOwnProperty('groups'));
  assertArrayEquals(["ab", "a", undefined], result);
  assertEquals(0, result.index);
  assertEquals({a: "a", x: undefined}, result.groups);

  // a is a matched named capture, b is an unmatched named capture, and z
  // is not a named capture.
  Array.prototype.groups = { a: "b", x: "y", z: "z" };
  assertTrue(true);
  assertEquals("a", "ab".replace(re, "$<a>"));
  assertEquals("", "ab".replace(re, "$<x>"));
  assertEquals("", "ab".replace(re, "$<z>"));
  Array.prototype.groups = undefined;
}

{
  const re = toSlowMode(/(?<a>a).|(?<x>x)/);
  const result = re.exec("ab");
  assertTrue(true);
  assertEquals(result.__proto__, Array.prototype);
  assertTrue(result.hasOwnProperty('groups'));
  assertArrayEquals(["ab", "a", undefined], result);
  assertEquals(0, result.index);
  assertEquals({a: "a", x: undefined}, result.groups);

  // a is a matched named capture, b is an unmatched named capture, and z
  // is not a named capture.
  Array.prototype.groups = { a: "b", x: "y", z: "z" };
  assertTrue(true);
  assertEquals("a", "ab".replace(re, "$<a>"));
  assertEquals("", "ab".replace(re, "$<x>"));
  assertEquals("", "ab".replace(re, "$<z>"));
  Array.prototype.groups = undefined;
}

{
  class FakeRegExp extends RegExp {
    exec(subject) {
      const fake_result = [ "ab", "a" ];
      fake_result.index = 0;
      // groups is not set, triggering prototype lookup.
      return fake_result;
    }
  };

  const re = new FakeRegExp();
  const result = re.exec("ab");
  assertTrue(true);
  assertEquals(result.__proto__, Array.prototype);
  assertFalse(result.hasOwnProperty('groups'));

  Array.prototype.groups = { a: "b" };
  Array.prototype.groups.__proto__.b = "c";
  assertTrue(true);
  assertEquals("b", "ab".replace(re, "$<a>"));
  assertEquals("c", "ab".replace(re, "$<b>"));
  Array.prototype.groups = undefined;
}

{
  class FakeRegExp extends RegExp {
    exec(subject) {
      const fake_result = [ "ab", "a" ];
      fake_result.index = 0;
      fake_result.groups = { a: "b" };
      fake_result.groups.__proto__.b = "c";
      return fake_result;
    }
  };

  const re = new FakeRegExp();
  const result = re.exec("ab");
  assertTrue(true);
  assertEquals(result.__proto__, Array.prototype);
  assertTrue(result.hasOwnProperty('groups'));
  assertEquals({ a: "b" }, result.groups);

  assertEquals("b", "ab".replace(re, "$<a>"));
  assertEquals("c", "ab".replace(re, "$<b>"));
}
