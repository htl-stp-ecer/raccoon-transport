import 'dart:async';
import 'package:build/build.dart';
import 'package:glob/glob.dart';

import 'src/lcm/parser/lexer.dart';
import 'src/lcm/parser/parser.dart';
import 'src/lcm/parser/token.dart';
import 'src/lcm/parser/ast.dart';
import 'src/lcm/generator/dart_generator.dart';

/// Builder that generates Dart code from LCM message definitions
class LcmBuilder implements Builder {
  @override
  final buildExtensions = const {
    '.lcm': ['.g.dart']
  };

  @override
  Future<void> build(BuildStep buildStep) async {
    final inputId = buildStep.inputId;
    final inputPath = inputId.path;

    final outputPath = inputPath.replaceAll('.lcm', '.g.dart');
    final outputId = AssetId(inputId.package, outputPath);

    final content = await buildStep.readAsString(inputId);

    try {
      final lexer = LcmLexer(content, inputPath);
      final tokens = lexer.tokenize();

      final parser = LcmParser(tokens, inputPath);
      final lcmFile = parser.parse();

      final generator = DartGenerator();

      await for (final assetId in buildStep.findAssets(Glob('**.lcm'))) {
        if (assetId == inputId) continue;
        try {
          final depContent = await buildStep.readAsString(assetId);
          final depLexer = LcmLexer(depContent, assetId.path);
          final depTokens = depLexer.tokenize();
          final depParser = LcmParser(depTokens, assetId.path);
          final depFile = depParser.parse();
          generator.registerStructs(depFile);
        } catch (e) {
          log.warning('Could not parse ${assetId.path} for type resolution: $e');
        }
      }

      final dartCode = generator.generate(lcmFile);

      await buildStep.writeAsString(outputId, dartCode);
      log.info('Generated ${outputId.path} from ${inputId.path}');
    } on LexerException catch (e) {
      log.severe('Lexer error in ${inputId.path}: $e');
    } on ParseException catch (e) {
      log.severe('Parse error in ${inputId.path}: $e');
    } catch (e, stack) {
      log.severe('Error generating ${inputId.path}: $e\n$stack');
    }
  }
}

Builder lcmBuilder(BuilderOptions options) => LcmBuilder();
