#!/usr/bin/env python3

#Copyright (C) 2009-2011 by Benedict Paten (benedictpaten@gmail.com)
#
#Released under the MIT license, see LICENSE.txt
import unittest
import sys

from sonLib.bioio import TestStatus

from cactus.shared.test import getCactusInputs_random
from cactus.shared.test import getCactusInputs_blanchette
from cactus.shared.test import runWorkflow_multipleExamples

class TestCase(unittest.TestCase):
    @TestStatus.mediumLength
    def testCactusNormalisation_Random(self):
        runWorkflow_multipleExamples(self.id(), getCactusInputs_random,
                                     testNumber=TestStatus.getTestSetup())

    @unittest.skip("test was never updated when changes were made to the way ancestors work (ERROR: Couldn't find reference event reference)")
    @TestStatus.needsTestData
    @TestStatus.shortLength
    def testCactusNormalisation_Blanchette(self):
        runWorkflow_multipleExamples(self.id(), getCactusInputs_blanchette)

if __name__ == '__main__':
    unittest.main()
