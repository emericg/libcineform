/*!
 * @file lutpath.h
 * @brief Active MetadataTools
 *
 * (C) Copyright 2017 GoPro Inc (http://gopro.com/).
 *
 * Licensed under either:
 * - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 * - MIT license, http://opensource.org/licenses/MIT
 * at your option.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void OverrideCFHDDATA(struct decoder *decoder, unsigned char *lpCurrentBuffer, int nWordsUsed);
void OverrideCFHDDATAUsingParent(struct decoder *decoder, struct decoder *parentDecoder, unsigned char *lpCurrentBuffer, int nWordsUsed);

#ifdef __cplusplus
}
#endif
