/*---
description: V8 mjsunit regexp.js, under forced time-travel
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

// ==== regexp.js ====
// Copyright 2012 the V8 project authors. All rights reserved.
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

// Flags: --allow-natives-syntax

function testEscape(str, regex) {
  assertEquals("foo:bar:baz", str.split(regex).join(":"));
}

testEscape("foo\nbar\nbaz", /\n/);
testEscape("foo bar baz", /\s/);
testEscape("foo\tbar\tbaz", /\s/);
testEscape("foo-bar-baz", /\u002D/);

// Test containing null char in regexp.
var s = '[' + String.fromCharCode(0) + ']';
var re = new RegExp(s);
assertEquals(s.match(re).length, 1);
assertEquals(s.match(re)[0], String.fromCharCode(0));

// Test strings containing all line separators
s = 'aA\nbB\rcC\r\ndD\u2028eE\u2029fF';
re = /^./gm; // any non-newline character at the beginning of a line
var result = s.match(re);
assertEquals(result.length, 6);
assertEquals(result[0], 'a');
assertEquals(result[1], 'b');
assertEquals(result[2], 'c');
assertEquals(result[3], 'd');
assertEquals(result[4], 'e');
assertEquals(result[5], 'f');

re = /.$/gm; // any non-newline character at the end of a line
result = s.match(re);
assertEquals(result.length, 6);
assertEquals(result[0], 'A');
assertEquals(result[1], 'B');
assertEquals(result[2], 'C');
assertEquals(result[3], 'D');
assertEquals(result[4], 'E');
assertEquals(result[5], 'F');

re = /^[^]/gm; // *any* character at the beginning of a line
result = s.match(re);
assertEquals(result.length, 7);
assertEquals(result[0], 'a');
assertEquals(result[1], 'b');
assertEquals(result[2], 'c');
assertEquals(result[3], '\n');
assertEquals(result[4], 'd');
assertEquals(result[5], 'e');
assertEquals(result[6], 'f');

re = /[^]$/gm; // *any* character at the end of a line
result = s.match(re);
assertEquals(result.length, 7);
assertEquals(result[0], 'A');
assertEquals(result[1], 'B');
assertEquals(result[2], 'C');
assertEquals(result[3], '\r');
assertEquals(result[4], 'D');
assertEquals(result[5], 'E');
assertEquals(result[6], 'F');

// Some tests from the Mozilla tests, where our behavior used to differ from
// SpiderMonkey.
// From ecma_3/RegExp/regress-334158.js
assertTrue(/\ca/.test( "\x01" ));
assertFalse(/\ca/.test( "\\ca" ));
assertFalse(/\ca/.test( "ca" ));
assertTrue(/\c[a/]/.test( "\\ca" ));
assertTrue(/\c[a/]/.test( "\\c/" ));

// Test \c in character class
re = /^[\cM]$/;
assertTrue(re.test("\r"));
assertFalse(re.test("M"));
assertFalse(re.test("c"));
assertFalse(re.test("\\"));
assertFalse(re.test("\x03"));  // I.e., read as \cc

re = /^[\c]]$/;
assertTrue(re.test("c]"));
assertTrue(re.test("\\]"));
assertFalse(re.test("\x1d"));  // ']' & 0x1f
assertFalse(re.test("\x03]"));  // I.e., read as \cc

re = /^[\c1]$/;  // Digit control characters are masked in character classes.
assertTrue(re.test("\x11"));
assertFalse(re.test("\\"));
assertFalse(re.test("c"));
assertFalse(re.test("1"));

re = /^[\c_]$/;  // Underscore control character is masked in character classes.
assertTrue(re.test("\x1f"));
assertFalse(re.test("\\"));
assertFalse(re.test("c"));
assertFalse(re.test("_"));

re = /^[\c$]$/;  // Other characters are interpreted literally.
assertFalse(re.test("\x04"));
assertTrue(re.test("\\"));
assertTrue(re.test("c"));
assertTrue(re.test("$"));

assertTrue(/^[Z-\c-e]*$/.test("Z[\\cde"));

// Test that we handle \s and \S correctly on special Unicode characters.
re = /\s/;
assertTrue(re.test("\u2028"));
assertTrue(re.test("\u2029"));
assertTrue(re.test("\uFEFF"));

re = /\S/;
assertFalse(re.test("\u2028"));
assertFalse(re.test("\u2029"));
assertFalse(re.test("\uFEFF"));

// Test that we handle \s and \S correctly inside some bizarre
// character classes.
re = /[\s-:]/;
assertTrue(re.test('-'));
assertTrue(re.test(':'));
assertTrue(re.test(' '));
assertTrue(re.test('\t'));
assertTrue(re.test('\n'));
assertFalse(re.test('a'));
assertFalse(re.test('Z'));

re = /[\S-:]/;
assertTrue(re.test('-'));
assertTrue(re.test(':'));
assertFalse(re.test(' '));
assertFalse(re.test('\t'));
assertFalse(re.test('\n'));
assertTrue(re.test('a'));
assertTrue(re.test('Z'));

re = /[^\s-:]/;
assertFalse(re.test('-'));
assertFalse(re.test(':'));
assertFalse(re.test(' '));
assertFalse(re.test('\t'));
assertFalse(re.test('\n'));
assertTrue(re.test('a'));
assertTrue(re.test('Z'));

re = /[^\S-:]/;
assertFalse(re.test('-'));
assertFalse(re.test(':'));
assertTrue(re.test(' '));
assertTrue(re.test('\t'));
assertTrue(re.test('\n'));
assertFalse(re.test('a'));
assertFalse(re.test('Z'));

re = /[\s]/;
assertFalse(re.test('-'));
assertFalse(re.test(':'));
assertTrue(re.test(' '));
assertTrue(re.test('\t'));
assertTrue(re.test('\n'));
assertFalse(re.test('a'));
assertFalse(re.test('Z'));

re = /[^\s]/;
assertTrue(re.test('-'));
assertTrue(re.test(':'));
assertFalse(re.test(' '));
assertFalse(re.test('\t'));
assertFalse(re.test('\n'));
assertTrue(re.test('a'));
assertTrue(re.test('Z'));

re = /[\S]/;
assertTrue(re.test('-'));
assertTrue(re.test(':'));
assertFalse(re.test(' '));
assertFalse(re.test('\t'));
assertFalse(re.test('\n'));
assertTrue(re.test('a'));
assertTrue(re.test('Z'));

re = /[^\S]/;
assertFalse(re.test('-'));
assertFalse(re.test(':'));
assertTrue(re.test(' '));
assertTrue(re.test('\t'));
assertTrue(re.test('\n'));
assertFalse(re.test('a'));
assertFalse(re.test('Z'));

re = /[\s\S]/;
assertTrue(re.test('-'));
assertTrue(re.test(':'));
assertTrue(re.test(' '));
assertTrue(re.test('\t'));
assertTrue(re.test('\n'));
assertTrue(re.test('a'));
assertTrue(re.test('Z'));

re = /[^\s\S]/;
assertFalse(re.test('-'));
assertFalse(re.test(':'));
assertFalse(re.test(' '));
assertFalse(re.test('\t'));
assertFalse(re.test('\n'));
assertFalse(re.test('a'));
assertFalse(re.test('Z'));

// First - is treated as range operator, second as literal minus.
// This follows the specification in parsing, but doesn't throw on
// the \s at the beginning of the range.
re = /[\s-0-9]/;
assertTrue(re.test(' '));
assertTrue(re.test('\xA0'));
assertTrue(re.test('-'));
assertTrue(re.test('0'));
assertTrue(re.test('9'));
assertFalse(re.test('1'));

// Test beginning and end of line assertions with or without the
// multiline flag.
re = /^\d+/;
assertFalse(re.test("asdf\n123"));
re = /^\d+/m;
assertTrue(re.test("asdf\n123"));

re = /\d+$/;
assertFalse(re.test("123\nasdf"));
re = /\d+$/m;
assertTrue(re.test("123\nasdf"));

// Test that empty matches are handled correctly for multiline global
// regexps.
re = /^(.*)/mg;
assertEquals(3, "a\n\rb".match(re).length);
assertEquals("*a\n*b\r*c\n*\r*d\r*\n*e", "a\nb\rc\n\rd\r\ne".replace(re, "*$1"));

// Test that empty matches advance one character
re = new RegExp("", "g");
assertEquals("xAx", "A".replace(re, "x"));
assertEquals(3, String.fromCharCode(161).replace(re, "x").length);

// Test that we match the KJS behavior with regard to undefined constructor
// arguments:
re = new RegExp();
// KJS actually shows this as '//'.  Here we match the Firefox behavior (ie,
// giving a syntactically legal regexp literal).
assertEquals('/(?:)/', re.toString());
re = new RegExp(void 0);
assertEquals('/(?:)/', re.toString());
re.compile();
assertEquals('/(?:)/', re.toString());
re.compile(void 0);
assertEquals('/(?:)/', re.toString());


// Check for early syntax errors.
assertThrows("/foo(/gi");

// Check $01 and $10
re = new RegExp("(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)");
assertEquals("t", "123456789t".replace(re, "$10"), "$10");
assertEquals("15", "123456789t".replace(re, "$15"), "$10");
assertEquals("1", "123456789t".replace(re, "$01"), "$01");
assertEquals("$001", "123456789t".replace(re, "$001"), "$001");
re = new RegExp("foo(.)");
assertEquals("bar$0", "foox".replace(re, "bar$0"), "$0");
assertEquals("bar$00", "foox".replace(re, "bar$00"), "$00");
assertEquals("bar$000", "foox".replace(re, "bar$000"), "$000");
assertEquals("barx", "foox".replace(re, "bar$01"), "$01 2");
assertEquals("barx5", "foox".replace(re, "bar$15"), "$15");

assertFalse(/()foo$\1/.test("football"), "football1");
assertFalse(/foo$(?=ball)/.test("football"), "football2");
assertFalse(/foo$(?!bar)/.test("football"), "football3");
assertTrue(/()foo$\1/.test("foo"), "football4");
assertTrue(/foo$(?=(ball)?)/.test("foo"), "football5");
assertTrue(/()foo$(?!bar)/.test("foo"), "football6");
assertFalse(/(x?)foo$\1/.test("football"), "football7");
assertFalse(/foo$(?=ball)/.test("football"), "football8");
assertFalse(/foo$(?!bar)/.test("football"), "football9");
assertTrue(/(x?)foo$\1/.test("foo"), "football10");
assertTrue(/foo$(?=(ball)?)/.test("foo"), "football11");
assertTrue(/foo$(?!bar)/.test("foo"), "football12");

// Check that the back reference has two successors.  See
// BackReferenceNode::PropagateForward.
assertFalse(/f(o)\b\1/.test('foo'));
assertTrue(/f(o)\B\1/.test('foo'));

// Back-reference, ignore case:
// ASCII
assertEquals("xaAx,a", String(/x(a)\1x/i.exec("xaAx")), "backref-ASCII");
assertFalse(/x(...)\1/i.test("xaaaaa"), "backref-ASCII-short");
assertTrue(/x((?:))\1\1x/i.test("xx"), "backref-ASCII-empty");
assertTrue(/x(?:...|(...))\1x/i.test("xabcx"), "backref-ASCII-uncaptured");
assertTrue(/x(?:...|(...))\1x/i.test("xabcABCx"), "backref-ASCII-backtrack");
assertEquals("xaBcAbCABCx,aBc",
             String(/x(...)\1\1x/i.exec("xaBcAbCABCx")),
             "backref-ASCII-twice");

for (var i = 0; i < 128; i++) {
  var testName = "backref-ASCII-char-" + i + "," + (i^0x20);
  var test = /^(.)\1$/i.test(String.fromCharCode(i, i ^ 0x20))
  var c = String.fromCharCode(i);
  if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z')) {
    assertTrue(test, testName);
  } else {
    assertFalse(test, testName);
  }
}

assertFalse(/f(o)$\1/.test('foo'), "backref detects at_end");

// Check decimal escapes doesn't overflow.
// (Note: \214 is interpreted as octal).
assertArrayEquals(["\x8c7483648"],
                  /\2147483648/.exec("\x8c7483648"),
                  "Overflow decimal escape");


// Check numbers in quantifiers doesn't overflow and doesn't throw on
// too large numbers.
assertFalse(/a{111111111111111111111111111111111111111111111}/.test('b'),
            "overlarge1");
assertFalse(/a{999999999999999999999999999999999999999999999}/.test('b'),
            "overlarge2");
assertFalse(/a{1,111111111111111111111111111111111111111111111}/.test('b'),
            "overlarge3");
assertFalse(/a{1,999999999999999999999999999999999999999999999}/.test('b'),
            "overlarge4");
assertFalse(/a{2147483648}/.test('b'),
            "overlarge5");
assertFalse(/a{21474836471}/.test('b'),
            "overlarge6");
assertFalse(/a{1,2147483648}/.test('b'),
            "overlarge7");
assertFalse(/a{1,21474836471}/.test('b'),
            "overlarge8");
assertFalse(/a{2147483648,2147483648}/.test('b'),
            "overlarge9");
assertFalse(/a{21474836471,21474836471}/.test('b'),
            "overlarge10");
assertFalse(/a{2147483647}/.test('b'),
            "overlarge11");
assertFalse(/a{1,2147483647}/.test('b'),
            "overlarge12");
assertTrue(/a{1,2147483647}/.test('a'),
            "overlarge13");
assertFalse(/a{2147483647,2147483647}/.test('a'),
            "overlarge14");


// Check that we don't read past the end of the string.
assertFalse(/f/.test('b'));
assertFalse(/[abc]f/.test('x'));
assertFalse(/[abc]f/.test('xa'));
assertFalse(/[abc]</.test('x'));
assertFalse(/[abc]</.test('xa'));
assertFalse(/f/i.test('b'));
assertFalse(/[abc]f/i.test('x'));
assertFalse(/[abc]f/i.test('xa'));
assertFalse(/[abc]</i.test('x'));
assertFalse(/[abc]</i.test('xa'));
assertFalse(/f[abc]/.test('x'));
assertFalse(/f[abc]/.test('xa'));
assertFalse(/<[abc]/.test('x'));
assertFalse(/<[abc]/.test('xa'));
assertFalse(/f[abc]/i.test('x'));
assertFalse(/f[abc]/i.test('xa'));
assertFalse(/<[abc]/i.test('x'));
assertFalse(/<[abc]/i.test('xa'));

// Test that merging of quick test masks gets it right.
assertFalse(/x([0-7]%%x|[0-6]%%y)/.test('x7%%y'), 'qt');
assertFalse(/()x\1(y([0-7]%%%x|[0-6]%%%y)|dkjasldkas)/.test('xy7%%%y'), 'qt2');
assertFalse(/()x\1(y([0-7]%%%x|[0-6]%%%y)|dkjasldkas)/.test('xy%%%y'), 'qt3');
assertFalse(/()x\1y([0-7]%%%x|[0-6]%%%y)/.test('xy7%%%y'), 'qt4');
assertFalse(/()x\1(y([0-7]%%%x|[0-6]%%%y)|dkjasldkas)/.test('xy%%%y'), 'qt5');
assertFalse(/()x\1y([0-7]%%%x|[0-6]%%%y)/.test('xy7%%%y'), 'qt6');
assertFalse(/xy([0-7]%%%x|[0-6]%%%y)/.test('xy7%%%y'), 'qt7');
assertFalse(/x([0-7]%%%x|[0-6]%%%y)/.test('x7%%%y'), 'qt8');


// Don't hang on this one.
/[^\xfe-\xff]*/.test("");


var long = "a";
for (var i = 0; i < 100000; i++) {
  long = "a?" + long;
}
// Don't crash on this one, but maybe throw an exception.
try {
  RegExp(long).exec("a");
} catch (e) {
  assertTrue(String(e).indexOf("too large") >= 0, "too large");
}


// Test that compile works on modified objects
var re = /re+/;
assertEquals("re+", re.source);
assertFalse(re.global);
assertFalse(re.ignoreCase);
assertFalse(re.multiline);
assertEquals(0, re.lastIndex);

re.compile("ro+", "gim");
assertEquals("ro+", re.source);
assertTrue(re.global);
assertTrue(re.ignoreCase);
assertTrue(re.multiline);
assertEquals(0, re.lastIndex);

re.lastIndex = 42;
re.someOtherProperty = 42;
re.someDeletableProperty = 42;
re[37] = 37;
re[42] = 42;

re.compile("ra+", "i");
assertEquals("ra+", re.source);
assertFalse(re.global);
assertTrue(re.ignoreCase);
assertFalse(re.multiline);
assertEquals(0, re.lastIndex);

assertEquals(42, re.someOtherProperty);
assertEquals(42, re.someDeletableProperty);
assertEquals(37, re[37]);
assertEquals(42, re[42]);

re.lastIndex = -1;
re.someOtherProperty = 37;
re[42] = 37;
assertTrue(delete re[37]);
assertTrue(delete re.someDeletableProperty);
re.compile("ri+", "gm");

assertEquals("ri+", re.source);
assertTrue(re.global);
assertFalse(re.ignoreCase);
assertTrue(re.multiline);
assertEquals(0, re.lastIndex);
assertEquals(37, re.someOtherProperty);
assertEquals(37, re[42]);

// Test boundary-checks.
function assertRegExpTest(re, input, test) {
  assertEquals(test, re.test(input), "test:" + re + ":" + input);
}

assertRegExpTest(/b\b/, "b", true);
assertRegExpTest(/b\b$/, "b", true);
assertRegExpTest(/\bb/, "b", true);
assertRegExpTest(/^\bb/, "b", true);
assertRegExpTest(/,\b/, ",", false);
assertRegExpTest(/,\b$/, ",", false);
assertRegExpTest(/\b,/, ",", false);
assertRegExpTest(/^\b,/, ",", false);

assertRegExpTest(/b\B/, "b", false);
assertRegExpTest(/b\B$/, "b", false);
assertRegExpTest(/\Bb/, "b", false);
assertRegExpTest(/^\Bb/, "b", false);
assertRegExpTest(/,\B/, ",", true);
assertRegExpTest(/,\B$/, ",", true);
assertRegExpTest(/\B,/, ",", true);
assertRegExpTest(/^\B,/, ",", true);

assertRegExpTest(/b\b/, "b,", true);
assertRegExpTest(/b\b/, "ba", false);
assertRegExpTest(/b\B/, "b,", false);
assertRegExpTest(/b\B/, "ba", true);

assertRegExpTest(/b\Bb/, "bb", true);
assertRegExpTest(/b\bb/, "bb", false);

assertRegExpTest(/b\b[,b]/, "bb", false);
assertRegExpTest(/b\B[,b]/, "bb", true);
assertRegExpTest(/b\b[,b]/, "b,", true);
assertRegExpTest(/b\B[,b]/, "b,", false);

assertRegExpTest(/[,b]\bb/, "bb", false);
assertRegExpTest(/[,b]\Bb/, "bb", true);
assertRegExpTest(/[,b]\bb/, ",b", true);
assertRegExpTest(/[,b]\Bb/, ",b", false);

assertRegExpTest(/[,b]\b[,b]/, "bb", false);
assertRegExpTest(/[,b]\B[,b]/, "bb", true);
assertRegExpTest(/[,b]\b[,b]/, ",b", true);
assertRegExpTest(/[,b]\B[,b]/, ",b", false);
assertRegExpTest(/[,b]\b[,b]/, "b,", true);
assertRegExpTest(/[,b]\B[,b]/, "b,", false);

// Test that caching of result doesn't share result objects.
// More iterations increases the chance of hitting a GC.
for (var i = 0; i < 100; i++) {
  var re = /x(y)z/;
  var res = re.exec("axyzb");
  assertTrue(!!res);
  assertEquals(2, res.length);
  assertEquals("xyz", res[0]);
  assertEquals("y", res[1]);
  assertEquals(1, res.index);
  assertEquals("axyzb", res.input);
  assertEquals(undefined, res.foobar);

  res.foobar = "Arglebargle";
  res[3] = "Glopglyf";
  assertEquals("Arglebargle", res.foobar);
}

// Test that we perform the spec required conversions in the correct order.
var log;
var string = "the string";
var fakeLastIndex = {
      valueOf: function() {
        log.push("li");
        return 0;
      }
    };
var fakeString = {
      toString: function() {
        log.push("ts");
        return string;
      },
      length: 0
    };

var re = /str/;
log = [];
re.lastIndex = fakeLastIndex;
var result = re.exec(fakeString);
assertEquals(["str"], result);
assertEquals(["ts", "li"], log);

// Again, to check if caching interferes.
log = [];
re.lastIndex = fakeLastIndex;
result = re.exec(fakeString);
assertEquals(["str"], result);
assertEquals(["ts", "li"], log);

// And one more time, just to be certain.
log = [];
re.lastIndex = fakeLastIndex;
result = re.exec(fakeString);
assertEquals(["str"], result);
assertEquals(["ts", "li"], log);

// Now with a global regexp, where lastIndex is actually used.
re = /str/g;
log = [];
re.lastIndex = fakeLastIndex;
var result = re.exec(fakeString);
assertEquals(["str"], result);
assertEquals(["ts", "li"], log);

// Again, to check if caching interferes.
log = [];
re.lastIndex = fakeLastIndex;
result = re.exec(fakeString);
assertEquals(["str"], result);
assertEquals(["ts", "li"], log);

// And one more time, just to be certain.
log = [];
re.lastIndex = fakeLastIndex;
result = re.exec(fakeString);
assertEquals(["str"], result);
assertEquals(["ts", "li"], log);


// Check that properties of RegExp have the correct permissions.
var re = /x/g;
var desc = Object.getOwnPropertyDescriptor(re.__proto__, "global");
assertInstanceof(desc.get, Function);
assertEquals(true, desc.configurable);
assertEquals(false, desc.enumerable);

desc = Object.getOwnPropertyDescriptor(re.__proto__, "multiline");
assertInstanceof(desc.get, Function);
assertEquals(true, desc.configurable);
assertEquals(false, desc.enumerable);

desc = Object.getOwnPropertyDescriptor(re.__proto__, "ignoreCase");
assertInstanceof(desc.get, Function);
assertEquals(true, desc.configurable);
assertEquals(false, desc.enumerable);

desc = Object.getOwnPropertyDescriptor(re, "global");
assertEquals(undefined, desc);

desc = Object.getOwnPropertyDescriptor(re, "multiline");
assertEquals(undefined, desc);

desc = Object.getOwnPropertyDescriptor(re, "ignoreCase");
assertEquals(undefined, desc);

desc = Object.getOwnPropertyDescriptor(re, "lastIndex");
assertEquals(0, desc.value);
assertEquals(false, desc.configurable);
assertEquals(false, desc.enumerable);
assertEquals(true, desc.writable);


// Check that end-anchored regexps are optimized correctly.
var re = /(?:a|bc)g$/;
assertTrue(re.test("ag"));
assertTrue(re.test("bcg"));
assertTrue(re.test("abcg"));
assertTrue(re.test("zimbag"));
assertTrue(re.test("zimbcg"));

assertFalse(re.test("g"));
assertFalse(re.test(""));

// Global regexp (non-zero start).
var re = /(?:a|bc)g$/g;
assertTrue(re.test("ag"));
re.lastIndex = 1;  // Near start of string.
assertTrue(re.test("zimbag"));
re.lastIndex = 6;  // At end of string.
assertFalse(re.test("zimbag"));
re.lastIndex = 5;  // Near end of string.
assertFalse(re.test("zimbag"));
re.lastIndex = 4;
assertTrue(re.test("zimbag"));

// Anchored at both ends.
var re = /^(?:a|bc)g$/g;
assertTrue(re.test("ag"));
re.lastIndex = 1;
assertFalse(re.test("ag"));
re.lastIndex = 1;
assertFalse(re.test("zag"));

// Long max_length of RegExp.
var re = /VeryLongRegExp!{1,1000}$/;
assertTrue(re.test("BahoolaVeryLongRegExp!!!!!!"));
assertFalse(re.test("VeryLongRegExp"));
assertFalse(re.test("!"));

// End anchor inside disjunction.
var re = /(?:a$|bc$)/;
assertTrue(re.test("a"));
assertTrue(re.test("bc"));
assertTrue(re.test("abc"));
assertTrue(re.test("zimzamzumba"));
assertTrue(re.test("zimzamzumbc"));
assertFalse(re.test("c"));
assertFalse(re.test(""));

// Only partially anchored.
var re = /(?:a|bc$)/;
assertTrue(re.test("a"));
assertTrue(re.test("bc"));
assertEquals(["a"], re.exec("abc"));
assertEquals(4, re.exec("zimzamzumba").index);
assertEquals(["bc"], re.exec("zimzomzumbc"));
assertFalse(re.test("c"));
assertFalse(re.test(""));

// Valid syntax in ES5.
re = RegExp("(?:x)*");
re = RegExp("(x)*");

// Syntax extension relative to ES5, for matching JSC (and ES3).
// Shouldn't throw.
re = RegExp("(?=x)*");
re = RegExp("(?!x)*");

// Should throw. Shouldn't hit asserts in debug mode.
assertThrows("RegExp('(*)')");
assertThrows("RegExp('(?:*)')");
assertThrows("RegExp('(?=*)')");
assertThrows("RegExp('(?!*)')");

// Test trimmed regular expression for RegExp.test().
assertTrue(/.*abc/.test("abc"));
assertFalse(/.*\d+/.test("q"));

// Test that RegExp.prototype.toString() throws TypeError for
// incompatible receivers (ES5 section 15.10.6 and 15.10.6.4).
assertThrows("RegExp.prototype.toString.call(null)", TypeError);
assertThrows("RegExp.prototype.toString.call(0)", TypeError);
assertThrows("RegExp.prototype.toString.call('')", TypeError);
assertThrows("RegExp.prototype.toString.call(false)", TypeError);
assertThrows("RegExp.prototype.toString.call(true)", TypeError);

// Test mutually recursive capture and backreferences.
assertEquals(["b", "", ""], /(\2)b(\1)/.exec("aba"));
assertEquals(["a", "", ""], /(\2).(\1)/.exec("aba"));
assertEquals(["aba", "a", "a"], /(.\2).(\1)/.exec("aba"));
assertEquals(["acbc", "c", "c"], /a(.\2)b(\1)$/.exec("acbc"));
assertEquals(["acbc", "c", "c"], /a(.\2)b(\1)/.exec("aabcacbc"));

// Test surrogate pair detection in split.
// \u{daff}\u{e000} is not a surrogate pair, while \u{daff}\u{dfff} is.
assertEquals(["\u{daff}", "\u{e000}"], "\u{daff}\u{e000}".split(/[a-z]{0,1}/u));
assertEquals(["\u{daff}\u{dfff}"], "\u{daff}\u{dfff}".split(/[a-z]{0,1}/u));

// Test that changing a property on RegExp.prototype results in us taking the
// slow path, which executes RegExp.prototype.exec instead of our
// RegExpExecStub.
const RegExpPrototypeExec = RegExp.prototype.exec;
RegExp.prototype.exec = function() { throw new Error(); }
assertThrows(() => "abc".replace(/./, ""));
RegExp.prototype.exec = RegExpPrototypeExec;

// Test the code path in RE.proto[@@search] when previousLastIndex is a receiver
// but can't be converted to a primitive. This exposed a crash in an older
// C++ implementation of @@search which a) still relied on Object::Equals,
// and b) incorrectly returned isolate->exception() on error.

var re = /./;
re.lastIndex = { [Symbol.toPrimitive]: 42 };
try { "abc".search(re); } catch (_) {}  // Ensure we don't crash.

// Test lastIndex values of -0.0 and NaN (since @@search uses SameValue).

var re = /./;
re.exec = function(str) { assertEquals(0, re.lastIndex); return []; }
re.lastIndex = -0.0;
assertEquals(-0, re.lastIndex);
"abc".search(re);
assertEquals(-0, re.lastIndex);

var re = /./;
re.exec = function(str) { assertEquals(0, re.lastIndex); return []; }
re.lastIndex = NaN;
assertEquals(NaN, re.lastIndex);
"abc".search(re);
assertEquals(NaN, re.lastIndex);

// Annex B changes: https://github.com/tc39/ecma262/pull/303

assertThrows("/{1}/", SyntaxError);
assertTrue(/^{*$/.test("{{{"));
assertTrue(/^}*$/.test("}}}"));
assertTrue(/]/.test("]"));
assertTrue(/^\c%$/.test("\\c%"));   // We go into ExtendedPatternCharacter.
assertTrue(/^\d%$/.test("2%"));     // ... CharacterClassEscape.
assertTrue(/^\e%$/.test("e%"));     // ... IdentityEscape.
assertTrue(/^\ca$/.test("\u{1}"));  // ... ControlLetter.
assertTrue(/^\cA$/.test("\u{1}"));  // ... ControlLetter.
assertTrue(/^\c9$/.test("\\c9"));   // ... ExtendedPatternCharacter.
assertTrue(/^\c$/.test("\\c"));   // ... ExtendedPatternCharacter.
assertTrue(/^[\c%]*$/.test("\\c%"));  // TODO(v8:6201): Not covered by the spec.
assertTrue(/^[\c:]*$/.test("\\c:"));  // TODO(v8:6201): Not covered by the spec.
assertTrue(/^[\c0]*$/.test("\u{10}"));  // ... ClassControlLetter.
assertTrue(/^[\c1]*$/.test("\u{11}"));  // ('0' % 32 == 0x10)
assertTrue(/^[\c2]*$/.test("\u{12}"));
assertTrue(/^[\c3]*$/.test("\u{13}"));
assertTrue(/^[\c4]*$/.test("\u{14}"));
assertTrue(/^[\c5]*$/.test("\u{15}"));
assertTrue(/^[\c6]*$/.test("\u{16}"));
assertTrue(/^[\c7]*$/.test("\u{17}"));
assertTrue(/^[\c8]*$/.test("\u{18}"));
assertTrue(/^[\c9]*$/.test("\u{19}"));
assertTrue(/^[\c_]*$/.test("\u{1F}"));
assertTrue(/^[\c11]*$/.test("\u{11}1"));
assertTrue(/^[\8]*$/.test("8"));  // ... ClassEscape ~~> IdentityEscape.
assertTrue(/^[\7]*$/.test("\u{7}"));  // ... ClassEscape
                                      // ~~> LegacyOctalEscapeSequence.
assertTrue(/^[\11]*$/.test("\u{9}"));
assertTrue(/^[\111]*$/.test("\u{49}"));
assertTrue(/^[\222]*$/.test("\u{92}"));
assertTrue(/^[\333]*$/.test("\u{DB}"));
assertTrue(/^[\444]*$/.test("\u{24}4"));
assertTrue(/^[\d-X]*$/.test("234-X-432"));  // CharacterRangeOrUnion.
assertTrue(/^[\d-X-Z]*$/.test("234-XZ-432"));
assertFalse(/^[\d-X-Z]*$/.test("234-XYZ-432"));

// Lone leading surrogates. Just here to exercise specific parsing code-paths.

assertFalse(/\uDB88|\uDBEC|aa/.test(""));
assertFalse(/\uDB88|\uDBEC|aa/u.test(""));

// EscapeRegExpPattern
assertEquals("\\n", /\n/.source);
assertEquals("\\n", new RegExp("\n").source);
assertEquals("\\n", new RegExp("\\n").source);
assertEquals("\\\\n", /\\n/.source);
assertEquals("\\r", /\r/.source);
assertEquals("\\r", new RegExp("\r").source);
assertEquals("\\r", new RegExp("\\r").source);
assertEquals("\\\\r", /\\r/.source);
assertEquals("\\u2028", /\u2028/.source);
assertEquals("\\u2028", new RegExp("\u2028").source);
assertEquals("\\u2028", new RegExp("\\u2028").source);
assertEquals("\\u2029", /\u2029/.source);
assertEquals("\\u2029", new RegExp("\u2029").source);
assertEquals("\\u2029", new RegExp("\\u2029").source);
assertEquals("[/]", /[/]/.source);
assertEquals("[\\/]", /[\/]/.source);
assertEquals("[\\\\/]", /[\\/]/.source);
assertEquals("[/]", new RegExp("[/]").source);
assertEquals("[/]", new RegExp("[\/]").source);
assertEquals("[\\/]", new RegExp("[\\/]").source);
assertEquals("[[/]", /[[/]/.source);
assertEquals("[/]]", /[/]]/.source);
assertEquals("[[/]]", /[[/]]/.source);
assertEquals("[[\\/]", /[[\/]/.source);
assertEquals("[[\\/]]", /[[\/]]/.source);
assertEquals("\\n", new RegExp("\\\n").source);
assertEquals("\\r", new RegExp("\\\r").source);
assertEquals("\\u2028", new RegExp("\\\u2028").source);
assertEquals("\\u2029", new RegExp("\\\u2029").source);

{
  // No escapes needed, the original string should be reused as `.source`.
  const pattern = "\\n";
  assertTrue(Object.is(pattern, new RegExp(pattern).source));
}

// Cons strings are flattened:
{
  let s = String.prototype.concat.call("aaaaaaaaaaaaaa", "bbbbbbbbbbbbbb");
  s = s.replace(/x.z/g, "");  // Prime the regexp.
  s = s.replace(/x.z/g, "");
}

// Unanchored two-alternative search where the first alternative's leading
// quick-check matches at a position but its body fails, while the second
// alternative matches there. The search-loop codegen must still try the second
// alternative at that position rather than committing to the first and
// advancing.
// Alternatives share the first four characters and differ only afterwards, so
// both have the same leading quick-check.
assertEquals("abcdf", /abcde|abcdf/.exec("abcdf")[0]);
assertEquals("abcde", /abcde|abcdf/.exec("abcde")[0]);
assertEquals("abcdf", /abcde|abcdf/.exec("xxabcdf")[0]);
assertNull(/abcde|abcdf/.exec("abcdx"));
// With a leading character class in the failing alternative.
assertEquals("bbcd", /[ad]bcd|bbcd/.exec("bbcd")[0]);
assertEquals("bdacbc", /bdacb[abc]c|bdacbc/.exec("bdacbc")[0]);
assertEquals("cccbd", /cccbd[ab]|cccbd/.exec("ddacccbd")[0]);
